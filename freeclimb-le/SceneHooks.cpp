// SceneHooks.cpp - passo 8. Ver SceneHooks.h.
//
// Posicoes e numero de argumentos (confirmados no despejo de funcoes do passo 7):
//   0x29 passagem "para baixo"      2 args    0x2A passagem "selecionada"  2 args
//   0x2B passagem "rigida"          2 args    0x2E so transformacao        1 arg
//   0x2D dados do mundo             1 arg
// Convencao: thiscall (this em ECX), a funcao chamada limpa a pilha ("ret N").

#include <math.h>
#include <string.h>
#include <windows.h>

#include "common/IDebugLog.h"
#include "SceneHooks.h"

namespace
{
	enum Table { kNiNode, kBSFadeNode, kFlatTree, kNiAVObject, kTables };
	enum Kind  { kDownward, kSelected, kRigid, kTransform, kWorld, kKinds };

	const unsigned	kVtable[kTables]	= { 0x011171A4, 0x0114D988, 0x011186A4, 0x011172B4 };
	const char *	kTableName[kTables]	= { "NiNode", "BSFadeNode", "BSFlattenedBoneTree", "NiAVObject" };
	const unsigned	kSlot[kKinds]		= { 0x29, 0x2A, 0x2B, 0x2E, 0x2D };
	const char *	kKindName[kKinds]	= { "downward(0x29)", "selected(0x2A)", "rigid(0x2B)", "transform(0x2E)", "world(0x2D)" };

	// So conta o esqueleto do jogador (a raiz e a tabela achatada, em separado):
	// contar todas as chamadas do jogo custava ~300 mil operacoes atomicas por segundo.
	volatile LONG		g_rootCalls[kKinds];
	volatile LONG		g_flatCalls[kKinds];
	volatile unsigned	g_playerRoot = 0, g_playerFlat = 0;
	volatile DWORD		g_threads[8];
	volatile LONG		g_threadCount = 0;
	DWORD				g_loadThread = 0;

	void NoteThread()
	{
		const DWORD id = GetCurrentThreadId();
		const LONG known = g_threadCount < 8 ? g_threadCount : 8;
		for(LONG i = 0; i < known; ++i)
			if(g_threads[i] == id) return;

		const LONG slot = InterlockedIncrement(&g_threadCount) - 1;
		if(slot < 8) g_threads[slot] = id;
	}

	inline void Count(int kind, void * self)
	{
		const unsigned s = (unsigned)self;
		if(s == g_playerFlat && s)		{ InterlockedIncrement(&g_flatCalls[kind]); NoteThread(); }
		else if(s == g_playerRoot && s)	{ InterlockedIncrement(&g_rootCalls[kind]); NoteThread(); }
	}

	// ---- experiencia de escrita (passo 10) ----
	volatile unsigned	g_headIndex = 0xFFFFFFFF, g_armIndex = 0xFFFFFFFF, g_activeIndex = 0xFFFFFFFF;
	volatile DWORD		g_expStart = 0, g_expEnd = 0;
	volatile LONG		g_inExp = 0, g_expApplied = 0, g_expSkipped = 0;

	bool ReadableMem(unsigned address, unsigned size)
	{
		if(address < 0x10000 || address + size < address) return false;
		unsigned p = address;
		const unsigned end = address + size;
		while(p < end)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if(!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) return false;
			if(mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
			const unsigned regionEnd = (unsigned)mbi.BaseAddress + (unsigned)mbi.RegionSize;
			if(regionEnd <= p) return false;
			p = regionEnd;
		}
		return true;
	}

	// Escreve a rotacao alterada na ENTRADA da tabela achatada do osso da cabeca (e nao no no):
	// quando um osso tem entrada, e a entrada que o jogo usa (no FreeClimb: local() = flat ? *flat : node->local).
	// Devolve true se escreveu (e preencheu 'saved' e 'entryOut').
	bool BeginExperiment(void * flat, float * saved, unsigned * entryOut)
	{
		const DWORD now = GetTickCount();
		if(!g_expStart || now < g_expStart || now > g_expEnd || g_activeIndex == 0xFFFFFFFF) return false;

		const unsigned f = (unsigned)flat;
		if(!ReadableMem(f + 0xB8, 12)) { InterlockedIncrement(&g_expSkipped); return false; }

		const unsigned count   = *(const unsigned *)(f + 0xB8);
		const unsigned entries = *(const unsigned *)(f + 0xC0);
		if(!entries || g_activeIndex >= count) { InterlockedIncrement(&g_expSkipped); return false; }

		const unsigned entry = entries + g_activeIndex * 0x78;
		if(!ReadableMem(entry, 0x78)) { InterlockedIncrement(&g_expSkipped); return false; }

		if(InterlockedCompareExchange(&g_inExp, 1, 0) != 0) return false;	// outra thread ja esta dentro

		float * rot = (float *)entry;										// NiTransform local: rotacao 3x3 em +0x00
		memcpy(saved, rot, 9 * sizeof(float));

		// movimento circular amplo (cone de ~40 graus, 0.5 Hz): Rz(a) * Ry(b)
		const float t = (float)(now - g_expStart) / 1000.0f;
		const float a = 0.7f * sinf(t * 3.1416f), bAngle = 0.7f * cosf(t * 3.1416f);
		const float ca = cosf(a), sa = sinf(a), cb = cosf(bAngle), sb = sinf(bAngle);
		const float Rz[9] = { ca, -sa, 0, sa, ca, 0, 0, 0, 1 };
		const float Ry[9] = { cb, 0, sb, 0, 1, 0, -sb, 0, cb };
		float R[9];
		for(int i = 0; i < 3; ++i)
			for(int j = 0; j < 3; ++j)
				R[i * 3 + j] = Rz[i * 3 + 0] * Ry[0 * 3 + j] + Rz[i * 3 + 1] * Ry[1 * 3 + j] + Rz[i * 3 + 2] * Ry[2 * 3 + j];

		float out[9];	// nova rotacao = antiga * R
		for(int i = 0; i < 3; ++i)
			for(int j = 0; j < 3; ++j)
				out[i * 3 + j] = saved[i * 3 + 0] * R[0 * 3 + j] + saved[i * 3 + 1] * R[1 * 3 + j] + saved[i * 3 + 2] * R[2 * 3 + j];

		memcpy(rot, out, 9 * sizeof(float));
		*entryOut = entry;
		InterlockedIncrement(&g_expApplied);
		return true;
	}

	void EndExperiment(unsigned entry, const float * saved)
	{
		if(ReadableMem(entry, 9 * sizeof(float))) memcpy((void *)entry, saved, 9 * sizeof(float));
		InterlockedExchange(&g_inExp, 0);
	}

	// ---- medicao: quanto mudou a rotacao MUNDIAL do osso do braco durante a escrita ----
	volatile float		g_maxEntryDeg = 0, g_maxNodeDeg = 0;
	float				g_snapEntry[9], g_snapNode[9];
	volatile LONG		g_snapValid = 0, g_measured = 0;

	float AngleDeg(const float * A, const float * B)
	{
		float tr = 0;
		for(int i = 0; i < 9; ++i) tr += A[i] * B[i];
		float c = (tr - 1.0f) * 0.5f;
		if(c > 1.0f) c = 1.0f;
		if(c < -1.0f) c = -1.0f;
		return acosf(c) * 57.29578f;
	}

	// Chamada logo depois da atualizacao do esqueleto do jogador.
	void Measure(unsigned flat, bool applied)
	{
		if(g_armIndex == 0xFFFFFFFF || !ReadableMem(flat + 0xB8, 12)) return;
		const unsigned count = *(const unsigned *)(flat + 0xB8), entries = *(const unsigned *)(flat + 0xC0);
		if(!entries || g_armIndex >= count) return;

		const unsigned e = entries + g_armIndex * 0x78;
		if(!ReadableMem(e, 0x78)) return;

		const float * entryWorld = (const float *)(e + 0x34);			// rotacao mundial da entrada
		const unsigned node = *(const unsigned *)(e + 0x70);
		const float * nodeWorld = (node && ReadableMem(node + 0x54, 36)) ? (const float *)(node + 0x54) : NULL;

		if(!applied)
		{
			// sem escrita: guarda a rotacao "normal" do osso (a referencia para comparar)
			memcpy(g_snapEntry, entryWorld, 36);
			if(nodeWorld) memcpy(g_snapNode, nodeWorld, 36); else memset(g_snapNode, 0, 36);
			g_snapValid = 1;
			return;
		}

		if(!g_snapValid) return;
		const float de = AngleDeg(g_snapEntry, entryWorld);
		const float dn = nodeWorld ? AngleDeg(g_snapNode, nodeWorld) : 0.0f;
		if(de > g_maxEntryDeg) g_maxEntryDeg = de;
		if(dn > g_maxNodeDeg) g_maxNodeDeg = dn;
		InterlockedIncrement(&g_measured);
	}

	typedef void (__thiscall * Fn2)(void *, void *, unsigned);	// (this, dados, flags)  ret 8
	typedef void (__thiscall * Fn1)(void *, void *);			// (this, dados)         ret 4

	template<int T, int K> struct Pass2
	{
		static Fn2 original;
		static void __fastcall call(void * self, void *, void * data, unsigned flags)
		{
			Count(K, self);
			original(self, data, flags);
		}
	};
	template<int T, int K> Fn2 Pass2<T, K>::original = NULL;

	// A passagem "para baixo" da tabela achatada leva a experiencia (so para o esqueleto do jogador).
	template<> struct Pass2<kFlatTree, kDownward>
	{
		static Fn2 original;
		static void __fastcall call(void * self, void *, void * data, unsigned flags)
		{
			Count(kDownward, self);

			float saved[9];
			unsigned entry = 0;
			const bool applied = ((unsigned)self == g_playerFlat && g_playerFlat) ? BeginExperiment(self, saved, &entry) : false;

			original(self, data, flags);

			if((unsigned)self == g_playerFlat && g_playerFlat) Measure((unsigned)self, applied);
			if(applied) EndExperiment(entry, saved);
		}
	};
	Fn2 Pass2<kFlatTree, kDownward>::original = NULL;

	template<int T, int K> struct Pass1
	{
		static Fn1 original;
		static void __fastcall call(void * self, void *, void * data)
		{
			Count(K, self);
			original(self, data);
		}
	};
	template<int T, int K> Fn1 Pass1<T, K>::original = NULL;

	// Escreve o ponteiro novo na vtable, de forma atomica, e devolve o antigo.
	template<class Fn> bool Patch(int table, int kind, void * hook, Fn * original)
	{
		volatile LONG * entry = (volatile LONG *)(kVtable[table] + 4 * kSlot[kind]);

		const unsigned previous = (unsigned)*entry;
		HMODULE owner = NULL;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   (LPCSTR)previous, &owner);
		const bool ours = owner == GetModuleHandleA(NULL);

		*original = (Fn)previous;

		DWORD old = 0;
		if(!VirtualProtect((LPVOID)entry, 4, PAGE_READWRITE, &old))
		{
			_MESSAGE("hooks: %s %s: VirtualProtect failed", kTableName[table], kKindName[kind]);
			return false;
		}
		InterlockedExchange(entry, (LONG)hook);
		VirtualProtect((LPVOID)entry, 4, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (LPCVOID)entry, 4);

		if(!ours) _MESSAGE("hooks: %s %s: the slot already belonged to another module (%08X) - chained", kTableName[table], kKindName[kind], previous);
		return true;
	}

	template<int T, int K> bool Install2() { return Patch(T, K, (void *)&Pass2<T, K>::call, &Pass2<T, K>::original); }
	template<int T, int K> bool Install1() { return Patch(T, K, (void *)&Pass1<T, K>::call, &Pass1<T, K>::original); }

	DWORD WINAPI ReportThread(LPVOID)
	{
		LONG lastRoot[kKinds] = { 0 }, lastFlat[kKinds] = { 0 };

		for(int round = 1; round <= 3; ++round)
		{
			Sleep(5000);

			char line[400];
			int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "hooks: after %d s, calls per second (root / flat tree):", round * 5);
			for(int k = 0; k < kKinds && n > 0; ++k)
			{
				const LONG r = g_rootCalls[k], f = g_flatCalls[k];
				n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "  %s %ld/%ld", kKindName[k], (r - lastRoot[k]) / 5, (f - lastFlat[k]) / 5);
				lastRoot[k] = r; lastFlat[k] = f;
			}
			_MESSAGE("%s", line);

			const LONG nThreads = g_threadCount < 8 ? g_threadCount : 8;
			char threads[160] = "";
			int m = 0;
			for(LONG i = 0; i < nThreads && m >= 0; ++i)
				m += _snprintf_s(threads + m, sizeof(threads) - m, _TRUNCATE, " %lu", (unsigned long)g_threads[i]);
			_MESSAGE("hooks:   threads that updated the player's skeleton:%s   (plugin was loaded on thread %lu)",
					 threads, (unsigned long)g_loadThread);
		}
		return 0;
	}
}

void InstallSceneHooks()
{
	g_loadThread = GetCurrentThreadId();

	// Os mesmos 16 pontos que o FreeClimb usa no SE: 3 tabelas x 3 passagens, 3 de transformacao, 4 de mundo.
	unsigned installed = 0;
	installed += Install2<kNiNode,     kDownward>();	installed += Install2<kNiNode,     kSelected>();	installed += Install2<kNiNode,     kRigid>();
	installed += Install2<kBSFadeNode, kDownward>();	installed += Install2<kBSFadeNode, kSelected>();	installed += Install2<kBSFadeNode, kRigid>();
	installed += Install2<kFlatTree,   kDownward>();	installed += Install2<kFlatTree,   kSelected>();	installed += Install2<kFlatTree,   kRigid>();

	installed += Install1<kNiNode,     kTransform>();
	installed += Install1<kBSFadeNode, kTransform>();
	installed += Install1<kFlatTree,   kTransform>();

	installed += Install1<kNiNode,     kWorld>();
	installed += Install1<kBSFadeNode, kWorld>();
	installed += Install1<kFlatTree,   kWorld>();
	installed += Install1<kNiAVObject, kWorld>();

	_MESSAGE("hooks: %u of 16 pass-through hooks installed (they only count calls)", installed);
}

void SceneHooksSetTarget(unsigned root, unsigned flat)
{
	g_playerRoot = root;
	g_playerFlat = flat;
	_MESSAGE("hooks: counting the player's skeleton: root=%08X flat=%08X", root, flat);
}

void SceneHooksStartReport()
{
	HANDLE t = CreateThread(NULL, 0, ReportThread, NULL, 0, NULL);
	if(t) CloseHandle(t);
}

namespace
{
	DWORD WINAPI CycleThread(LPVOID)
	{
		Sleep(8000);

		for(int cycle = 1; cycle <= 6; ++cycle)
		{
			const DWORD now = GetTickCount();
			g_activeIndex = g_armIndex;
			g_expApplied = 0; g_expSkipped = 0; g_maxEntryDeg = 0; g_maxNodeDeg = 0; g_measured = 0;
			g_expStart = now; g_expEnd = now + 6000;
			_MESSAGE("experiment: cycle %d ON  - the LEFT ARM should swing in circles for 6 s", cycle);

			Sleep(6500);
			_MESSAGE("experiment: cycle %d OFF - writes applied %ld, skipped %ld, measured %ld; max change of the arm's WORLD rotation: entry.world %.1f deg, node.world %.1f deg",
					 cycle, g_expApplied, g_expSkipped, g_measured, (double)g_maxEntryDeg, (double)g_maxNodeDeg);

			Sleep(6000);
		}
		_MESSAGE("experiment: all cycles done");
		return 0;
	}
}

void SceneHooksStartExperiment(unsigned headIndex, unsigned armIndex)
{
	static bool started = false;
	g_headIndex = headIndex;
	g_armIndex = armIndex;

	if(armIndex == 0xFFFFFFFF) { _MESSAGE("experiment: 'NPC L UpperArm [LUar]' not found - not started"); return; }

	_MESSAGE("experiment: automatic, left upper arm = flat entry %u. In ~8 s the arm swings 6 s / rests 6 s, 6 times. Stand still in third person and watch the left arm.", armIndex);
	if(!started)
	{
		started = true;
		HANDLE t = CreateThread(NULL, 0, CycleThread, NULL, 0, NULL);
		if(t) CloseHandle(t);
	}
}
