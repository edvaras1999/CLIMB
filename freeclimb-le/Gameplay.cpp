// Gameplay.cpp - passo 19. Reutiliza o codigo verificado do Freerun LE (Engine.h, Havok.h) para os raios,
// o controlador do personagem e as unidades do Havok, e o motor de poses do FreeClimb (SceneHooks) para o desenho.

#include <math.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Engine.h"
#include "Gameplay.h"
#include "Havok.h"
#include "Log.h"
#include "SceneHooks.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "src/Core.h"

#include "skse/GameInput.h"
#include "skse/GameReferences.h"

namespace
{
	UInt32 * const kJumpHandler		= (UInt32 *)0x010D460C;
	UInt32 * const kMovementHandler	= (UInt32 *)0x010D4594;
	enum { kSlot_CanProcess = 1, kSlot_ProcessButton = 4 };

	typedef bool (__thiscall * CanProcess_t)(void *, InputEvent *);
	typedef void (__thiscall * ProcessButton_t)(void *, ButtonEvent *, void *);

	CanProcess_t		g_origJumpCan = NULL, g_origMoveCan = NULL;
	ProcessButton_t		g_origJumpButton = NULL;

	volatile LONG		g_climbing = 0;
	volatile DWORD		g_climbStarted = 0;
	float				g_destX = 0, g_destY = 0, g_destZ = 0;

	// motion 42 = contextMantle (subir por cima de uma borda). Deslocamento do centro do corpo no fim do clipe,
	// em relacao ao repouso: ~62.9 para a frente (y) e ~132.3 para cima (z).
	const int			kMantle = 42;
	const float			kMantleForward = 62.9f, kMantleUp = 132.3f;
	const float			kMinHeight = 80.0f, kMaxHeight = 180.0f;		// bordas alcancaveis (unidades do Skyrim)

	NiPoint3 Add(const NiPoint3 & a, const NiPoint3 & b)	{ return NiPoint3(a.x + b.x, a.y + b.y, a.z + b.z); }
	NiPoint3 Scale(const NiPoint3 & a, float s)				{ return NiPoint3(a.x * s, a.y * s, a.z * s); }

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

	class TraversalWorld : public fc::World
	{
	public:
		explicit TraversalWorld(Actor * actor) : m_actor(actor) {}

		std::optional<fc::Hit> ray(fc::Vec from, fc::Vec to) override
		{
			const fc::Vec delta = to - from;
			const float distance = delta.length();
			if(!m_actor || !from.finite() || !to.finite() || distance <= 0.001f) return {};

			const Havok::Result hit = Havok::RayCast(
				NiPoint3(from.x, from.y, from.z),
				NiPoint3(delta.x, delta.y, delta.z),
				distance, Havok::kLayer_Transparent, m_actor);
			if(!hit.didHit) return {};

			return fc::Hit{
				fc::Vec{hit.hitPosition.x, hit.hitPosition.y, hit.hitPosition.z},
				fc::Vec{hit.normal.x, hit.normal.y, hit.normal.z},
				true
			};
		}

	private:
		Actor * m_actor;
	};

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

	// Usa a busca de bordas do FreeClimb quando a sonda LE simples nao encontra uma.
	// A subida atual continua sendo reproduzida pelo motor de poses LE e permanece como fallback.
	bool FindTraversalLedge(Actor * player, float fx, float fy, Ledge * out)
	{
		TraversalWorld world(player);
		fc::Traversal traversal;
		traversal.cfg.approachSeconds = 0.0f;
		traversal.cfg.staminaEnabled = false;

		const NiPoint3 feet = Engine::Position(player);
		if(!traversal.attach(world, fc::Vec{feet.x, feet.y, feet.z},
							 fc::Vec{fx, fy, 0.0f}, 1000.0f, 90.0f))
			return false;

		fc::Input input;
		input.y = 1.0f;
		input.mantle = true;
		for(int i = 0; i < 3; ++i)
		{
			const fc::Result result = traversal.update(world, input, 0.05f, 1000.0f);
			if(traversal.state == fc::State::mantle)
			{
				const fc::Vec target = traversal.topTarget();
				const float dx = target.x - feet.x;
				const float dy = target.y - feet.y;
				const float forward = dx * fx + dy * fy;
				const float side = -dx * fy + dy * fx;
				const float height = target.z - feet.z;
				if(!target.finite() || !isfinite(forward) || !isfinite(side) || !isfinite(height) ||
					forward < 20.0f || forward > 180.0f || fabsf(side) > 1.5f ||
					height < kMinHeight || height > kMaxHeight)
					return false;

				out->wallDistance = forward;
				out->height = height;
				out->topZ = target.z;
				out->forwardX = fx;
				out->forwardY = fy;
				return true;
			}
			if(result.released || !traversal.active()) return false;
		}
		return false;
	}

	// Chamada no fim da animacao (na thread que atualiza o esqueleto): coloca o personagem em cima da borda.
	void OnClimbFinished()
	{
		Actor * player = (Actor *)Engine::Player();
		void * ctrl = player ? Engine::CharController(player) : NULL;
		if(ctrl)
		{
			const float scale = Engine::HavokScale();
			__declspec(align(16)) float target[4];
			target[0] = g_destX * scale;
			target[1] = g_destY * scale;
			target[2] = (g_destZ + 1.0f) * scale;
			target[3] = 0.0f;
			Engine::ControllerSetPosition(ctrl, target, true, true);

			__declspec(align(16)) float stop[4] = { 0, 0, 0, 0 };
			Engine::ControllerSetLinearVelocity(ctrl, stop);
		}
		_MESSAGE("gameplay: climb finished - player placed on top at (%.0f %.0f %.0f)", (double)g_destX, (double)g_destY, (double)g_destZ);
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
			if(strcmp(reason, "no wall ahead") != 0)
			{
				_MESSAGE("gameplay: jump pressed near a wall but no climb: %s", reason);
				return false;
			}

			float fx, fy;
			if(!SceneHooksRootForward(&fx, &fy) || !FindTraversalLedge(player, fx, fy, &ledge))
			{
				_MESSAGE("gameplay: jump pressed near a wall but no climb: %s", reason);
				return false;
			}
			_MESSAGE("gameplay: FreeClimb traversal search found a ledge missed by the legacy probe");
		}

		// o clipe sobe 132.3 e avanca 62.9: ajusta a escala ao tamanho real da borda e a distancia a parede
		const float scaleZ = ledge.height / kMantleUp;
		const float scaleY = (ledge.wallDistance + 40.0f) / kMantleForward;

		const NiPoint3 feet = Engine::Position(player);
		g_destX = feet.x + ledge.forwardX * (ledge.wallDistance + 40.0f);
		g_destY = feet.y + ledge.forwardY * (ledge.wallDistance + 40.0f);
		g_destZ = ledge.topZ;

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
		}
		return g_climbing != 0;
	}

	bool __fastcall Hook_JumpCanProcess(void * handler, void *, InputEvent * event)
	{
		if(Climbing()) return false;
		return g_origJumpCan(handler, event);
	}

	bool __fastcall Hook_MoveCanProcess(void * handler, void *, InputEvent * event)
	{
		if(Climbing()) return false;
		return g_origMoveCan(handler, event);
	}

	void __fastcall Hook_JumpButton(void * handler, void *, ButtonEvent * event, void * data)
	{
		if(event && !Climbing())
		{
			const float value = *(float *)&event->flags;
			const bool isDown = (value != 0.0f) && (event->timer == 0.0f);
			if(isDown && TryStartClimb()) return;		// a pressao foi "consumida": nao ha salto
		}
		g_origJumpButton(handler, event, data);
	}
}

void InstallGameplay()
{
	PatchSlot(kJumpHandler,		kSlot_CanProcess,	(void *)&Hook_JumpCanProcess,	&g_origJumpCan);
	PatchSlot(kMovementHandler,	kSlot_CanProcess,	(void *)&Hook_MoveCanProcess,	&g_origMoveCan);
	PatchSlot(kJumpHandler,		kSlot_ProcessButton,(void *)&Hook_JumpButton,		&g_origJumpButton);
	_MESSAGE("gameplay: installed - press JUMP next to a wall 80 to 180 units high (about 1.1 to 2.5 m) to climb it");
}
