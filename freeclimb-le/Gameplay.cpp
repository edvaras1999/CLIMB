// Gameplay.cpp - liga a deteccao legacy de bordas aos controlos, Havok e poses do Skyrim LE.

#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Engine.h"
#include "Gameplay.h"
#include "Havok.h"
#include "Log.h"
#include "SceneHooks.h"
#include "TraversalWorld.h"
#include "skse/GameInput.h"
#include "skse/GameReferences.h"

namespace
{
	UInt32 * const kJumpHandler		= (UInt32 *)0x010D460C;
	enum { kSlot_CanProcess = 1, kSlot_ProcessButton = 4 };

	typedef bool (__thiscall * CanProcess_t)(void *, InputEvent *);
	typedef void (__thiscall * ProcessButton_t)(void *, ButtonEvent *, void *);

	CanProcess_t		g_origJumpCan = NULL;
	ProcessButton_t		g_origJumpButton = NULL;

	volatile LONG		g_climbing = 0;
	volatile DWORD		g_climbStarted = 0;
	float				g_destX = 0, g_destY = 0, g_destZ = 0;
	volatile LONG		g_jumpConsumedUntilRelease = 0;
	volatile LONG		g_climbCooldownUntil = 0;

	// motion 42 = contextMantle (subir por cima de uma borda). Deslocamento do centro do corpo no fim do clipe,
	// em relacao ao repouso: ~62.9 para a frente (y) e ~132.3 para cima (z).
	const int			kMantle = 42;
	const float			kMantleForward = 62.9f, kMantleUp = 132.3f;
	const float			kMinHeight = 80.0f, kMaxHeight = 180.0f;		// bordas alcancaveis (unidades do Skyrim)
	const DWORD			kClimbCooldownMs = 750;
	const float			kWorldHorizontalLimit = 100000.0f, kWorldVerticalMin = -10000.0f, kWorldVerticalMax = 30000.0f;
	const float			kMantleHorizontalLimit = 180.0f, kMantleVerticalMin = 40.0f, kMantleVerticalMax = 220.0f;

	NiPoint3 Add(const NiPoint3 & a, const NiPoint3 & b)	{ return NiPoint3(a.x + b.x, a.y + b.y, a.z + b.z); }

	template<class Fn> void PatchSlot(UInt32 * vtable, int slot, void * hook, Fn * original)
	{
		volatile LONG * entry = (volatile LONG *)(vtable + slot);
		*original = (Fn)*entry;

		DWORD old = 0;
		if(!VirtualProtect((LPVOID)entry, 4, PAGE_READWRITE, &old)) return;
		InterlockedExchange(entry, (LONG)hook);
		VirtualProtect((LPVOID)entry, 4, old, &old);
	}

	struct Ledge
	{
		float	wallDistance;		// do personagem a parede
		float	height;				// da altura dos pes ao topo da borda
		float	topZ;
		float	forwardX, forwardY;	// direcao (plana, unitaria)
	};

	void StartClimbCooldown()
	{
		InterlockedExchange(&g_climbCooldownUntil, (LONG)(GetTickCount() + kClimbCooldownMs));
	}

	bool SafeWorldPosition(const NiPoint3 & position)
	{
		return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
			std::abs(position.x) <= kWorldHorizontalLimit &&
			std::abs(position.y) <= kWorldHorizontalLimit &&
			position.z >= kWorldVerticalMin && position.z <= kWorldVerticalMax;
	}

	bool ClimbCooldownActive()
	{
		const DWORD until = (DWORD)InterlockedCompareExchange(&g_climbCooldownUntil, 0, 0);
		return (LONG)(until - GetTickCount()) > 0;
	}

	void StopPlayerVelocity(Actor * player)
	{
		void * ctrl = player ? Engine::CharController(player) : NULL;
		if(!ctrl) return;
		__declspec(align(16)) float stop[4] = { 0, 0, 0, 0 };
		Engine::ControllerSetLinearVelocity(ctrl, stop);
	}

	// Raios do Havok: parede a frente, superficie de cima, espaco livre por cima.
	bool FindLedge(Actor * player, Ledge * out, const char ** reason)
	{
		float fx, fy;
		if(!SceneHooksRootForward(&fx, &fy)) { *reason = "no skeleton direction yet"; return false; }

		const NiPoint3 feet = Engine::Position(player);
		const NiPoint3 fwd(fx, fy, 0.0f);
		const UInt32 layer = Havok::kLayer_Transparent;

		// 1) a parede: tres raios a alturas diferentes, o mais proximo que seja uma superficie vertical virada para nos
		bool found = false;
		float wallDist = 1e9f;
		for(int k = 0; k < 3; ++k)
		{
			static const float heights[3] = { 50.0f, 95.0f, 140.0f };
			const Havok::Result r = Havok::RayCast(Add(feet, NiPoint3(0, 0, heights[k])), fwd, 110.0f, layer, player);
			if(!r.didHit) continue;
			if(fabsf(r.normal.z) > 0.45f) continue;										// nao e uma parede
			if(r.normal.x * fwd.x + r.normal.y * fwd.y > -0.3f) continue;				// nao esta virada para nos
			if(r.distance < wallDist) { wallDist = r.distance; found = true; }
		}
		if(!found) { *reason = "no wall ahead"; return false; }

		// 2) o topo: de cima para baixo, um pouco para la da parede
		const float reach = wallDist + 25.0f;
		const NiPoint3 above(feet.x + fwd.x * reach, feet.y + fwd.y * reach, feet.z + 260.0f);
		const Havok::Result top = Havok::RayCast(above, NiPoint3(0, 0, -1), 260.0f, layer, player);
		if(!top.didHit || top.normal.z < 0.7f) { *reason = "no flat top behind the wall"; return false; }

		const float height = top.hitPosition.z - feet.z;
		if(height < kMinHeight) { *reason = "ledge too low"; return false; }
		if(height > kMaxHeight) { *reason = "ledge too high"; return false; }

		// 3) espaco livre por cima da borda (para a cabeca)
		const Havok::Result clear = Havok::RayCast(Add(top.hitPosition, NiPoint3(0, 0, 8.0f)), NiPoint3(0, 0, 1), 110.0f, layer, player);
		if(clear.didHit && clear.distance < 100.0f) { *reason = "no head room on top"; return false; }

		out->wallDistance = wallDist;
		out->height = height;
		out->topZ = top.hitPosition.z;
		out->forwardX = fx;
		out->forwardY = fy;
		return true;
	}

	// Chamada no fim da animacao (na thread que atualiza o esqueleto): coloca o personagem em cima da borda.
	void OnClimbFinished()
	{
		if(!InterlockedCompareExchange(&g_climbing, 0, 0))
		{
			_MESSAGE("gameplay: ignored a stale legacy mantle completion callback");
			return;
		}

		Actor * player = (Actor *)Engine::Player();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		bool placed = false;
		if(ctrl)
		{
			const NiPoint3 current = Engine::Position(player);
			const NiPoint3 mantleTarget(g_destX, g_destY, g_destZ + 1.0f);
			const float horizontal = std::hypot(mantleTarget.x - current.x, mantleTarget.y - current.y);
			const float vertical = mantleTarget.z - current.z;
			if(!SafeWorldPosition(mantleTarget) || !std::isfinite(horizontal) || !std::isfinite(vertical) ||
				horizontal > kMantleHorizontalLimit ||
				vertical < kMantleVerticalMin || vertical > kMantleVerticalMax)
			{
				_MESSAGE("gameplay: legacy mantle completion rejected unsafe target delta (xy %.1f z %.1f)",
						 (double)horizontal, (double)vertical);
				StopPlayerVelocity(player);
				InterlockedExchange(&g_climbing, 0);
				StartClimbCooldown();
				return;
			}

			const float scale = Engine::HavokScale();
			__declspec(align(16)) float target[4];
			target[0] = g_destX * scale;
			target[1] = g_destY * scale;
			target[2] = (g_destZ + 1.0f) * scale;
			target[3] = 0.0f;
			Engine::ControllerSetPosition(ctrl, target, true, true);

			__declspec(align(16)) float stop[4] = { 0, 0, 0, 0 };
			Engine::ControllerSetLinearVelocity(ctrl, stop);
			placed = true;
		}
		_MESSAGE("gameplay: climb finished - player %s at (%.0f %.0f %.0f)", placed ? "placed on top" : "not repositioned (controller unavailable)", (double)g_destX, (double)g_destY, (double)g_destZ);
		StartClimbCooldown();
		InterlockedExchange(&g_climbing, 0);
	}

	bool TryStartClimb()
	{
		Actor * player = (Actor *)Engine::Player();
		if(!player || SceneHooksIsPlaying()) return false;
		if(Engine::PlayerIsInMidair()) return false;

		void * ctrl = Engine::CharController(player);
		if(!ctrl || Engine::ControllerState(ctrl) != Engine::Controller::kState_OnGround) return false;

		Ledge ledge;
		const char * reason = "";
		if(!FindLedge(player, &ledge, &reason))
		{
			_MESSAGE("gameplay: legacy ledge probe found no mantle target: %s", reason);
			return false;
		}

		// o clipe sobe 132.3 e avanca 62.9: ajusta a escala ao tamanho real da borda e a distancia a parede
		const float scaleZ = ledge.height / kMantleUp;
		const float scaleY = (ledge.wallDistance + 40.0f) / kMantleForward;

		const NiPoint3 feet = Engine::Position(player);
		g_destX = feet.x + ledge.forwardX * (ledge.wallDistance + 40.0f);
		g_destY = feet.y + ledge.forwardY * (ledge.wallDistance + 40.0f);
		g_destZ = ledge.topZ;
		const float horizontal = std::hypot(g_destX - feet.x, g_destY - feet.y);
		const float vertical = g_destZ - feet.z;
		if(!SafeWorldPosition(NiPoint3(g_destX, g_destY, g_destZ)) ||
			!std::isfinite(horizontal) || !std::isfinite(vertical) ||
			horizontal > kMantleHorizontalLimit || vertical < kMinHeight || vertical > kMaxHeight)
		{
			_MESSAGE("gameplay: legacy ledge target rejected outside safe bounds (xy %.1f z %.1f)",
					 (double)horizontal, (double)vertical);
			return false;
		}

		if(!SceneHooksPlayMotion(kMantle, scaleY, scaleZ, &OnClimbFinished))
		{
			_MESSAGE("gameplay: ledge found (%.0f units high) but the pose engine is not ready yet", (double)ledge.height);
			return false;
		}

		g_climbStarted = GetTickCount();
		InterlockedExchange(&g_climbing, 1);
		_MESSAGE("gameplay: JUMP pressed - ledge found: wall %.0f units ahead, top %.0f units above the feet -> climbing (scale up %.2f, forward %.2f)",
				 (double)ledge.wallDistance, (double)ledge.height, (double)scaleZ, (double)scaleY);
		return true;
	}

	// seguranca: se por algum motivo a subida nunca terminar, nao deixar o personagem preso
	bool Climbing()
	{
		if(g_climbing && GetTickCount() - g_climbStarted > 6000)
		{
			_MESSAGE("gameplay: climb timed out - releasing the controls");
			InterlockedExchange(&g_climbing, 0);
			SceneHooksStopMotion();
			StartClimbCooldown();
		}
		return g_climbing != 0;
	}

	bool __fastcall Hook_JumpCanProcess(void * handler, void *, InputEvent * event)
	{
		return g_origJumpCan(handler, event);
	}

	void __fastcall Hook_JumpButton(void * handler, void *, ButtonEvent * event, void * data)
	{
		if(event)
		{
			const float value = *(float *)&event->flags;
			if(value == 0.0f)
			{
				InterlockedExchange(&g_jumpConsumedUntilRelease, 0);
				g_origJumpButton(handler, event, data);
				return;
			}

			if(InterlockedCompareExchange(&g_jumpConsumedUntilRelease, 0, 0))
				return;

			if(Climbing())
			{
				InterlockedExchange(&g_jumpConsumedUntilRelease, 1);
				return;
			}

			if(event->timer == 0.0f)
			{
				if(!ClimbCooldownActive() && TryStartClimb())
				{
					InterlockedExchange(&g_jumpConsumedUntilRelease, 1);
					return;
				}
			}
		}
		g_origJumpButton(handler, event, data);
	}
}

void InstallGameplay()
{
	PatchSlot(kJumpHandler,		kSlot_CanProcess,	(void *)&Hook_JumpCanProcess,	&g_origJumpCan);
	PatchSlot(kJumpHandler,		kSlot_ProcessButton,(void *)&Hook_JumpButton,		&g_origJumpButton);
	_MESSAGE("gameplay: installed - legacy wall/top probes only; press JUMP near a valid ledge for the safe mantle");
}
