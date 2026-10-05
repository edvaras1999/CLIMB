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

	// ---- experiencia: LEVITACAO escrevendo as posicoes MUNDIAIS, depois da atualizacao ----
	// Descoberto no passo 15 (0ABDEC0): no fim da passagem o jogo COPIA a transformacao mundial de cada
	// no para a entrada da tabela achatada. Por isso escrever na rotacao "local" da entrada nao serve
	// (e so um espelho). Aqui, depois da passagem, somamos uma altura a TODAS as posicoes mundiais
	// (entrada +0x60 e no +0x80), no meio da pose: o corpo todo sobe, sem contas de hierarquia.
	volatile DWORD		g_expStart = 0, g_expEnd = 0;
	volatile LONG		g_levFrames = 0, g_persisted = 0, g_rewritten = 0;
	float				g_lastDz = 0, g_lastZE = 0;
	volatile unsigned	g_comIndex = 0xFFFFFFFF;
	const float			kLiftUnits = 80.0f;			// ~1,1 m (1 unidade do Skyrim ~ 1,4 cm)

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

	bool FlatArray(unsigned flat, unsigned * entries, unsigned * count)
	{
		if(!ReadableMem(flat + 0xB8, 12)) return false;
		*count = *(const unsigned *)(flat + 0xB8);
		*entries = *(const unsigned *)(flat + 0xC0);
		return *entries && *count > 0 && *count <= 1024;
	}

	// Soma 'dz' a altura mundial de todas as entradas e de todos os nos que elas apontam.
	void AddDz(unsigned flat, float dz)
	{
		unsigned entries, count;
		if(!FlatArray(flat, &entries, &count)) return;

		for(unsigned i = 0; i < count; ++i)
		{
			const unsigned e = entries + i * 0x78;
			if(!ReadableMem(e, 0x78)) return;

			*(float *)(e + 0x60) += dz;										// entrada: world translate z

			const unsigned node = *(const unsigned *)(e + 0x70);
			if(node && ReadableMem(node + 0x80, 4)) *(float *)(node + 0x80) += dz;	// no: world translate z
		}
	}

	// Antes da atualizacao: se a nossa soma do frame anterior ainda la esta, desfaz-a (senao acumularia);
	// se o jogo ja reescreveu os valores, nao toca. Conta quantas vezes aconteceu cada caso.
	void PrePass(unsigned flat)
	{
		if(g_lastDz == 0.0f) return;

		unsigned entries, count;
		if(FlatArray(flat, &entries, &count) && g_comIndex < count && ReadableMem(entries + g_comIndex * 0x78, 0x78))
		{
			const float z = *(const float *)(entries + g_comIndex * 0x78 + 0x60);
			if(fabsf(z - g_lastZE) < 0.01f) { AddDz(flat, -g_lastDz); InterlockedIncrement(&g_persisted); }
			else							  { InterlockedIncrement(&g_rewritten); }
		}
		g_lastDz = 0.0f;
	}

	// Depois da atualizacao: durante a janela da experiencia, ergue o corpo todo (sobe e desce uma vez em 6 s).
	void PostPass(unsigned flat)
	{
		const DWORD now = GetTickCount();
		if(!g_expStart || now < g_expStart || now > g_expEnd) return;

		float t = (float)(now - g_expStart) / 6000.0f;
		if(t > 1.0f) t = 1.0f;
		const float dz = kLiftUnits * sinf(t * 3.14159265f);
		if(dz < 0.05f) return;

		unsigned entries, count;
		if(!FlatArray(flat, &entries, &count) || g_comIndex >= count) return;

		AddDz(flat, dz);
		g_lastDz = dz;
		g_lastZE = *(const float *)(entries + g_comIndex * 0x78 + 0x60);
		InterlockedIncrement(&g_levFrames);
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
			const bool player = ((unsigned)self == g_playerFlat && g_playerFlat);

			if(player) PrePass((unsigned)self);
			original(self, data, flags);
			if(player) PostPass((unsigned)self);
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
	// Posicao mundial do no raiz do jogador (translacao em +0x78).
	bool PlayerPos(float * p)
	{
		const unsigned root = g_playerRoot;
		if(!root || !ReadableMem(root + 0x78, 12)) return false;
		memcpy(p, (const void *)(root + 0x78), 12);
		return true;
	}

	DWORD WINAPI CycleThread(LPVOID)
	{
		// So comeca quando o jogador ANDA uns passos e depois PARA: assim nunca corre durante um
		// ecra de loading (la a posicao nao muda). Saltos grandes de posicao (teletransporte do
		// loading) sao ignorados: so conta andar de forma gradual.
		_MESSAGE("experiment: waiting - WALK a few steps (about 1 m or more) and then STAND STILL for 3 s. It never starts during a loading screen.");

		const DWORD t0 = GetTickCount();
		float last[3] = { 0, 0, 0 };
		bool haveLast = false, announced = false;
		float walked = 0.0f;
		DWORD stillSince = GetTickCount();

		for(;;)
		{
			Sleep(100);
			if(GetTickCount() - t0 > 600000) { _MESSAGE("experiment: no walking detected for 10 min - giving up"); return 0; }

			float p[3];
			if(!PlayerPos(p)) continue;
			if(!haveLast) { memcpy(last, p, 12); haveLast = true; stillSince = GetTickCount(); continue; }

			const float step = sqrtf((p[0] - last[0]) * (p[0] - last[0]) + (p[1] - last[1]) * (p[1] - last[1]));
			if(step > 0.5f && step < 60.0f) walked += step;			// andar: poucos centimetros por 0,1 s
			if(step > 1.0f) stillSince = GetTickCount();				// ainda a mexer (ou a saltar de sitio)
			memcpy(last, p, 12);

			if(walked > 80.0f && !announced) { announced = true; _MESSAGE("experiment: walking detected - gameplay is live. Now stand still."); }
			if(announced && GetTickCount() - stillSince > 3000) break;
		}

		for(int cycle = 1; cycle <= 4; ++cycle)
		{
			const DWORD now = GetTickCount();
			g_levFrames = 0; g_persisted = 0; g_rewritten = 0;
			g_expStart = now; g_expEnd = now + 6000;
			_MESSAGE("experiment: cycle %d ON  - the WHOLE BODY should rise about 1 metre and come back down over 6 s (levitation)", cycle);

			Sleep(6500);
			_MESSAGE("experiment: cycle %d OFF - frames written %ld; next frame still had our value (we undid it) %ld times, game had rewritten it %ld times",
					 cycle, g_levFrames, g_persisted, g_rewritten);

			Sleep(8000);
		}
		_MESSAGE("experiment: all cycles done");
		return 0;
	}
}

void SceneHooksStartExperiment(unsigned comIndex)
{
	static bool started = false;
	g_comIndex = comIndex;

	if(comIndex == 0xFFFFFFFF) { _MESSAGE("experiment: 'NPC COM [COM ]' not found - not started"); return; }

	_MESSAGE("experiment: automatic, centre-of-body bone = flat entry %u. In ~8 s the whole body should float up ~1 m and back, 4 times (6 s up-and-down, 8 s rest). Stand still in third person.", comIndex);
	if(!started)
	{
		started = true;
		HANDLE t = CreateThread(NULL, 0, CycleThread, NULL, 0, NULL);
		if(t) CloseHandle(t);
	}
}
