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
#include "PackLoader.h"
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

	// ---- Passo 17: aplicar uma POSE REAL do FreeClimb ao esqueleto do jogador ----
	// O jogo, no fim da passagem (0ABDEC0), copia a transformacao mundial de cada no para a tabela achatada,
	// e o desenho le essas posicoes. Por isso: DEPOIS da passagem, recalculamos as posicoes mundiais da
	// hierarquia inteira (645 entradas, pais antes dos filhos) e escrevemo-las na entrada e no no.
	// Para os ossos canonicos a rotacao local vem da pose; o resto mantem a transformacao local atual.
	volatile DWORD		g_poseStart = 0, g_poseEnd = 0;
	volatile LONG		g_poseReady = 0, g_poseFrames = 0;
	float				g_pose[99 * 10];
	int					g_boneEntry[99];
	volatile LONG		g_fkCheckRequested = 0, g_fkCheckDone = 0;
	float				g_fkMaxT = 0, g_fkMaxRotDeg = 0;
	unsigned			g_fkEntries = 0, g_fkMismatch = 0;
	volatile LONG		g_inPose = 0;

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

	struct Tr { float r[9]; float t[3]; float s; };		// NiTransform: rotacao 3x3 (linhas), translacao, escala

	// como o jogo multiplica transformacoes: R = Rp*Rl ; T = Tp + sp*(Rp*Tl) ; S = sp*sl
	void Mul(const Tr & p, const Tr & l, Tr * out)
	{
		for(int i = 0; i < 3; ++i)
			for(int j = 0; j < 3; ++j)
				out->r[i * 3 + j] = p.r[i * 3 + 0] * l.r[0 * 3 + j] + p.r[i * 3 + 1] * l.r[1 * 3 + j] + p.r[i * 3 + 2] * l.r[2 * 3 + j];
		for(int i = 0; i < 3; ++i)
			out->t[i] = p.t[i] + p.s * (p.r[i * 3 + 0] * l.t[0] + p.r[i * 3 + 1] * l.t[1] + p.r[i * 3 + 2] * l.t[2]);
		out->s = p.s * l.s;
	}

	// quaternio (x y z w) -> matriz; as COLUNAS sao os eixos rodados (a mesma convencao do FreeClimb/NiMatrix3)
	void FromQuat(const float * q, float * r)
	{
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		r[0] = 1 - 2 * (y * y + z * z);	r[1] = 2 * (x * y - z * w);		r[2] = 2 * (x * z + y * w);
		r[3] = 2 * (x * y + z * w);		r[4] = 1 - 2 * (x * x + z * z);	r[5] = 2 * (y * z - x * w);
		r[6] = 2 * (x * z - y * w);		r[7] = 2 * (y * z + x * w);		r[8] = 1 - 2 * (x * x + y * y);
	}

	float RotAngleDeg(const float * a, const float * b)
	{
		float tr = 0;
		for(int i = 0; i < 9; ++i) tr += a[i] * b[i];
		float c = (tr - 1.0f) * 0.5f;
		if(c > 1.0f) c = 1.0f;
		if(c < -1.0f) c = -1.0f;
		return acosf(c) * 57.29578f;
	}

	// A transformacao local ATUAL de uma entrada: do no, se tem no (o no e o que o jogo anima); senao da entrada.
	void LiveLocal(unsigned e, Tr * out)
	{
		const unsigned node = *(const unsigned *)(e + 0x70);
		if(node && ReadableMem(node + 0x20, 0x34))
		{
			memcpy(out->r, (const void *)(node + 0x20), 36);
			memcpy(out->t, (const void *)(node + 0x44), 12);
			out->s = *(const float *)(node + 0x50);
		}
		else
		{
			memcpy(out->r, (const void *)(e + 0x00), 36);
			memcpy(out->t, (const void *)(e + 0x24), 12);
			out->s = *(const float *)(e + 0x30);
		}
	}

	void ReadWorld(unsigned base, unsigned rotOff, unsigned tOff, unsigned sOff, Tr * out)
	{
		memcpy(out->r, (const void *)(base + rotOff), 36);
		memcpy(out->t, (const void *)(base + tOff), 12);
		out->s = *(const float *)(base + sOff);
	}

	void WriteWorld(unsigned base, unsigned rotOff, unsigned tOff, unsigned sOff, const Tr & w)
	{
		memcpy((void *)(base + rotOff), w.r, 36);
		memcpy((void *)(base + tOff), w.t, 12);
		*(float *)(base + sOff) = w.s;
	}

	Tr g_world[1024];

	// Calcula as posicoes mundiais de todas as entradas. Se 'usePose', os ossos canonicos recebem a rotacao da pose.
	bool ComputeWorlds(unsigned flat, bool usePose, unsigned entries, unsigned count)
	{
		// de que osso canonico e cada entrada (-1 = nenhum)
		int boneOfEntry[1024];
		for(unsigned i = 0; i < count; ++i) boneOfEntry[i] = -1;
		if(usePose)
			for(int b = 0; b < 99; ++b)
				if(g_boneEntry[b] >= 0 && (unsigned)g_boneEntry[b] < count) boneOfEntry[g_boneEntry[b]] = b;

		Tr root;
		ReadWorld(flat, 0x54, 0x78, 0x84, &root);		// a propria tabela e o "pai" das entradas de topo

		for(unsigned i = 0; i < count; ++i)
		{
			const unsigned e = entries + i * 0x78;
			if(!ReadableMem(e, 0x78)) return false;

			Tr local;
			LiveLocal(e, &local);
			if(boneOfEntry[i] >= 0) FromQuat(g_pose + boneOfEntry[i] * 10 + 3, local.r);	// rotacao da pose; t e s ficam os do esqueleto real

			const int parent = (int)(short)(*(const unsigned short *)(e + 0x68));
			if(parent >= (int)i) return false;											// ordem inesperada: nao arriscar
			Mul(parent >= 0 ? g_world[parent] : root, local, &g_world[i]);
		}
		return true;
	}

	// Depois da atualizacao do esqueleto do jogador.
	void PostPass(unsigned flat)
	{
		const DWORD now = GetTickCount();
		const bool poseOn = g_poseReady && g_poseStart && now >= g_poseStart && now <= g_poseEnd;
		if(!poseOn && !g_fkCheckRequested) return;

		unsigned entries, count;
		if(!FlatArray(flat, &entries, &count)) return;
		if(InterlockedCompareExchange(&g_inPose, 1, 0) != 0) return;

		if(poseOn)
		{
			if(ComputeWorlds(flat, true, entries, count))
			{
				for(unsigned i = 0; i < count; ++i)
				{
					const unsigned e = entries + i * 0x78;
					WriteWorld(e, 0x34, 0x58, 0x64, g_world[i]);						// entrada: mundo em +0x34
					const unsigned node = *(const unsigned *)(e + 0x70);
					if(node && ReadableMem(node + 0x54, 0x34)) WriteWorld(node, 0x54, 0x78, 0x84, g_world[i]);	// no: mundo em +0x54
				}
				InterlockedIncrement(&g_poseFrames);
			}
		}
		else if(g_fkCheckRequested && ComputeWorlds(flat, false, entries, count))
		{
			// auto-verificacao: o que calculamos a partir das locais bate com o que o jogo calculou?
			float maxT = 0, maxRot = 0; unsigned bad = 0;
			for(unsigned i = 0; i < count; ++i)
			{
				const unsigned e = entries + i * 0x78;
				Tr live; ReadWorld(e, 0x34, 0x58, 0x64, &live);
				float dt = 0;
				for(int k = 0; k < 3; ++k) { const float d = fabsf(live.t[k] - g_world[i].t[k]); if(d > dt) dt = d; }
				const float dr = RotAngleDeg(live.r, g_world[i].r);
				if(dt > maxT) maxT = dt;
				if(dr > maxRot) maxRot = dr;
				if(dt > 0.5f || dr > 1.0f) ++bad;
			}
			g_fkMaxT = maxT; g_fkMaxRotDeg = maxRot; g_fkEntries = count; g_fkMismatch = bad;
			InterlockedExchange(&g_fkCheckRequested, 0);
			InterlockedExchange(&g_fkCheckDone, 1);
		}

		InterlockedExchange(&g_inPose, 0);
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
		_MESSAGE("pose test: waiting - WALK a few steps (about 1 m or more) and then STAND STILL for 3 s. It never starts during a loading screen.");

		const DWORD t0 = GetTickCount();
		float last[3] = { 0, 0, 0 };
		bool haveLast = false, announced = false;
		float walked = 0.0f;
		DWORD stillSince = GetTickCount();

		for(;;)
		{
			Sleep(100);
			if(GetTickCount() - t0 > 600000) { _MESSAGE("pose test: no walking detected for 10 min - giving up"); return 0; }

			float p[3];
			if(!PlayerPos(p)) continue;
			if(!haveLast) { memcpy(last, p, 12); haveLast = true; stillSince = GetTickCount(); continue; }

			const float step = sqrtf((p[0] - last[0]) * (p[0] - last[0]) + (p[1] - last[1]) * (p[1] - last[1]));
			if(step > 0.5f && step < 60.0f) walked += step;
			if(step > 1.0f) stillSince = GetTickCount();
			memcpy(last, p, 12);

			if(walked > 80.0f && !announced) { announced = true; _MESSAGE("pose test: walking detected - gameplay is live. Now stand still."); }
			if(announced && GetTickCount() - stillSince > 3000) break;
		}

		// 1) auto-verificacao da matematica (nao altera nada)
		g_fkCheckDone = 0;
		InterlockedExchange(&g_fkCheckRequested, 1);
		for(int w = 0; w < 100 && !g_fkCheckDone; ++w) Sleep(100);
		if(g_fkCheckDone)
			_MESSAGE("pose test: FK self-check on %u entries: max position error %.3f units, max rotation error %.2f deg, %u entries off by more than 0.5 units / 1 deg",
					 g_fkEntries, (double)g_fkMaxT, (double)g_fkMaxRotDeg, g_fkMismatch);
		else
			_MESSAGE("pose test: FK self-check did not run (no pass seen)");

		// 2) as poses, uma de cada vez: 6 s a aplicar, 8 s de descanso
		const int nPoses = FC_PoseCount();
		_MESSAGE("pose test: %d poses will be applied one at a time (6 s each, 8 s rest). Watch the character from third person.", nPoses);
		for(int k = 0; k < nPoses; ++k)
		{
			g_poseReady = 0;
			if(!FC_GetPose(k, g_pose)) { _MESSAGE("pose test: pose %d '%s' could not be read - skipped", k, FC_PoseName(k)); continue; }
			g_poseReady = 1;

			const DWORD now = GetTickCount();
			g_poseFrames = 0; g_poseStart = now; g_poseEnd = now + 6000;
			_MESSAGE("pose test: pose %d ON  - '%s'", k, FC_PoseName(k));
			Sleep(6500);
			_MESSAGE("pose test: pose %d OFF - %ld frames written", k, g_poseFrames);
			g_poseStart = 0;
			Sleep(8000);
		}
		_MESSAGE("pose test: all poses done");
		return 0;
	}
}

void SceneHooksBindBones(const int * entryIndex)
{
	unsigned bound = 0;
	for(int b = 0; b < 99; ++b)
	{
		g_boneEntry[b] = entryIndex[b];
		if(entryIndex[b] >= 0) ++bound;
	}
	_MESSAGE("pose test: %u of 99 canonical bones are bound to flat-table entries (engine-owned and optional bones are left to the game)", bound);
}

void SceneHooksStartPoseTest()
{
	static bool started = false;
	if(started) return;
	started = true;

	if(FC_PoseCount() == 0) { _MESSAGE("pose test: the animation library is not loaded - not started"); return; }

	HANDLE t = CreateThread(NULL, 0, CycleThread, NULL, 0, NULL);
	if(t) CloseHandle(t);
}
