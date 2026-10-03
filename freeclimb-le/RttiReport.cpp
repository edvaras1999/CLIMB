// RttiReport.cpp - passo 3. Le a informacao de tipos (RTTI) do TESV.exe ja em memoria.
// So LE memoria do proprio executavel; nao altera nada no jogo.

#include <windows.h>
#include <shlobj.h>
#include <string.h>
#include <string>
#include <vector>

#include "common/IDebugLog.h"
#include "RttiReport.h"
#include "VtableScan.h"

namespace
{
	struct Wanted
	{
		const char *	mangled;	// nome decorado do MSVC
		const char *	label;
		unsigned		known;		// vtable ja verificada no Freerun LE (0 = ainda desconhecida)
	};

	const Wanted kWanted[] =
	{
		// --- ja conhecidas do Freerun LE: servem de prova de que o metodo acerta ---
		{ ".?AVActivateHandler@@",		"ActivateHandler",		0x010D453C },
		{ ".?AVTogglePOVHandler@@",		"TogglePOVHandler",		0x010D4574 },
		{ ".?AVMovementHandler@@",		"MovementHandler",		0x010D4594 },
		{ ".?AVLookHandler@@",			"LookHandler",			0x010D45AC },
		{ ".?AVReadyWeaponHandler@@",	"ReadyWeaponHandler",	0x010D45C4 },
		{ ".?AVJumpHandler@@",			"JumpHandler",			0x010D460C },
		{ ".?AVSneakHandler@@",			"SneakHandler",			0x010D4644 },

		// --- que o FreeClimb tambem usa e ainda nao temos ---
		{ ".?AVSprintHandler@@",		"SprintHandler",		0 },
		{ ".?AVAttackBlockHandler@@",	"AttackBlockHandler",	0 },
		{ ".?AVShoutHandler@@",			"ShoutHandler",			0 },
		{ ".?AVAutoMoveHandler@@",		"AutoMoveHandler",		0 },
		{ ".?AVRunHandler@@",			"RunHandler",			0 },
		{ ".?AVToggleRunHandler@@",		"ToggleRunHandler",		0 },

		// --- objetos do jogo ---
		{ ".?AVPlayerCharacter@@",		"PlayerCharacter",		0 },
		{ ".?AVActor@@",				"Actor",				0 },
		{ ".?AVNiAVObject@@",			"NiAVObject",			0 },
		{ ".?AVNiNode@@",				"NiNode",				0 },
		{ ".?AVBSFadeNode@@",			"BSFadeNode",			0 },
		{ ".?AVBSFlattenedBoneTree@@",	"BSFlattenedBoneTree",	0 },
		{ ".?AVBSWin32KeyboardDevice@@","BSWin32KeyboardDevice",0 },
		{ ".?AVBSPCGamepadDeviceHandler@@","BSPCGamepadDeviceHandler",0 },

		// --- camera: o Freerun LE usa a vtable de entrada destes dois estados ---
		{ ".?AVThirdPersonState@@",		"ThirdPersonState",		0x010E29D4 },
		{ ".?AVFirstPersonState@@",		"FirstPersonState",		0x010E30B0 },
	};

	// Junta os blocos de memoria legiveis do executavel e o intervalo de codigo.
	void AddReadable(std::vector<vt::Region> & regions, unsigned start, unsigned size)
	{
		unsigned p = start;
		const unsigned end = start + size;

		while(p < end)
		{
			MEMORY_BASIC_INFORMATION mbi;
			if(!VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi))) break;

			const unsigned regionEnd = (unsigned)mbi.BaseAddress + (unsigned)mbi.RegionSize;
			const unsigned chunkEnd = regionEnd < end ? regionEnd : end;
			if(chunkEnd <= p) break;

			const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
			if(readable)
			{
				vt::Region r;
				r.address	= p;
				r.size		= chunkEnd - p;
				r.data		= (const unsigned char *)p;
				regions.push_back(r);
			}

			p = chunkEnd;
		}
	}

	bool Collect(std::vector<vt::Region> & regions, unsigned & codeLo, unsigned & codeHi)
	{
		const BYTE * base = (const BYTE *)GetModuleHandleA(NULL);
		if(!base) return false;

		const IMAGE_DOS_HEADER *	dos	= (const IMAGE_DOS_HEADER *)base;
		const IMAGE_NT_HEADERS32 *	nt	= (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
		const IMAGE_SECTION_HEADER * sec = IMAGE_FIRST_SECTION(nt);

		codeLo = 0xFFFFFFFF;
		codeHi = 0;

		for(unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			const unsigned start	= (unsigned)base + sec[i].VirtualAddress;
			const unsigned size		= sec[i].Misc.VirtualSize;

			if(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
			{
				if(start < codeLo)			codeLo = start;
				if(start + size > codeHi)	codeHi = start + size;
			}
			else if(sec[i].Characteristics & IMAGE_SCN_MEM_READ)
			{
				AddReadable(regions, start, size);
			}
		}

		return codeHi > codeLo && !regions.empty();
	}

	void LogNamed(const char * prefix, const vt::Named & n)
	{
		_MESSAGE("rtti: %s%08X  slots=%-3u offset=%-4d %s", prefix, n.vtable, n.slots, n.offset, n.name);
	}

	// Descobre os NOMES reais das classes, quando os que procuramos nao batem.
	void DiscoverNames(const std::vector<vt::Region> & regions, unsigned codeLo, unsigned codeHi)
	{
		// A) as vtables que o Freerun LE ja verificou: que classe sao, de facto?
		_MESSAGE("rtti: --- A) what the Freerun LE vtables really are ---");
		for(size_t w = 0; w < sizeof(kWanted) / sizeof(kWanted[0]); ++w)
		{
			if(!kWanted[w].known) continue;
			vt::Named n;
			if(vt::Describe(regions, codeLo, codeHi, kWanted[w].known, n))
				LogNamed("", n);
			else
				_MESSAGE("rtti: %08X  (not a vtable with RTTI)  [expected %s]", kWanted[w].known, kWanted[w].label);
		}

		// B) tudo o que existe na vizinhanca dessas vtables (onde ficam os outros tratadores)
		_MESSAGE("rtti: --- B) every vtable between 010D4300 and 010D4800 ---");
		const std::vector<vt::Named> range = vt::DumpRange(regions, codeLo, codeHi, 0x010D4300, 0x010D4800);
		for(size_t i = 0; i < range.size(); ++i) LogNamed("", range[i]);
		_MESSAGE("rtti: (%u vtables)", (unsigned)range.size());

		// C) classes que nao apareceram, procuradas por parte do nome
		const char * const parts[] = { "FadeNode", "Gamepad", "XInput", "Node@@" };
		for(size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); ++p)
		{
			_MESSAGE("rtti: --- C) names containing \"%s\" ---", parts[p]);
			const std::vector<vt::Named> hits = vt::FindByNameContaining(regions, codeLo, codeHi, parts[p], 70);
			for(size_t i = 0; i < hits.size(); ++i) LogNamed("", hits[i]);
			if(hits.empty()) _MESSAGE("rtti: (none)");
		}
	}

	// ---- passo 4: despejar o inicio de cada funcao virtual para um ficheiro de texto ----
	void AppendHex(std::string & out, const unsigned char * p, unsigned n)
	{
		static const char * const digits = "0123456789ABCDEF";
		for(unsigned i = 0; i < n; ++i)
		{
			out += digits[p[i] >> 4];
			out += digits[p[i] & 15];
		}
	}

	struct DumpTarget
	{
		const char *	mangled;
		const char *	label;
		unsigned		bytes;		// quantos bytes de cada funcao
	};

	const DumpTarget kDump[] =
	{
		{ ".?AVNiAVObject@@",				"NiAVObject",				192 },
		{ ".?AVNiNode@@",					"NiNode",					192 },
		{ ".?AVBSFlattenedBoneTree@@",		"BSFlattenedBoneTree",		192 },
		{ ".?AVPlayerCharacter@@",			"PlayerCharacter",			128 },
		{ ".?AVActor@@",					"Actor",					128 },
		{ ".?AVBSWin32KeyboardDevice@@",	"BSWin32KeyboardDevice",	128 },
		{ ".?AVBSWin32GamepadDevice@@",		"BSWin32GamepadDevice",		128 },
		{ ".?AVBSGamepadDevice@@",			"BSGamepadDevice",			128 },
		{ ".?AVThirdPersonState@@",			"ThirdPersonState",			128 },
		{ ".?AVFirstPersonState@@",			"FirstPersonState",			128 },
		{ ".?AVPlayerInputHandler@@",		"PlayerInputHandler",		128 },
	};

	void DumpVtablesToFile(const std::vector<vt::Region> & regions, unsigned codeLo, unsigned codeHi)
	{
		std::string out;
		out.reserve(300 * 1024);
		out += "# FreeClimb LE - vtable dump (passo 4)\n# formato: slot  endereco-da-funcao  bytes-iniciais-em-hex\n";
		out += "# EXT = o slot aponta para fora do TESV.exe (outro plugin interceptou-o)\n";

		unsigned classes = 0, functions = 0, external = 0;

		for(size_t k = 0; k < sizeof(kDump) / sizeof(kDump[0]); ++k)
		{
			const vt::Result r = vt::Find(regions, codeLo, codeHi, kDump[k].mangled);

			const vt::Found * primary = NULL;
			for(size_t v = 0; v < r.vtables.size(); ++v)
				if(r.vtables[v].offset == 0) { primary = &r.vtables[v]; break; }

			char head[200];
			if(!primary)
			{
				_snprintf_s(head, sizeof(head), _TRUNCATE, "== %s NOT FOUND\n", kDump[k].label);
				out += head;
				continue;
			}

			++classes;
			_snprintf_s(head, sizeof(head), _TRUNCATE, "== %s vtable=%08X slots=%u bytes=%u\n",
						kDump[k].label, primary->vtable, primary->slots, kDump[k].bytes);
			out += head;

			for(unsigned s = 0; s < primary->slots; ++s)
			{
				const unsigned fn = *(const unsigned *)(primary->vtable + 4 * s);
				char line[64];
				++functions;

				if(fn >= codeLo && fn + kDump[k].bytes <= codeHi)
				{
					_snprintf_s(line, sizeof(line), _TRUNCATE, "%u %08X ", s, fn);
					out += line;
					AppendHex(out, (const unsigned char *)fn, kDump[k].bytes);
					out += "\n";
				}
				else
				{
					++external;
					_snprintf_s(line, sizeof(line), _TRUNCATE, "%u %08X EXT\n", s, fn);
					out += line;
				}
			}
		}

		char path[MAX_PATH];
		if(FAILED(SHGetFolderPathA(NULL, CSIDL_MYDOCUMENTS, NULL, SHGFP_TYPE_CURRENT, path)))
		{
			_MESSAGE("dump: could not find the Documents folder");
			return;
		}
		strcat_s(path, sizeof(path), "\\My Games\\Skyrim\\SKSE\\FreeClimbLE_vtables.txt");

		HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if(file == INVALID_HANDLE_VALUE)
		{
			_MESSAGE("dump: could not create %s", path);
			return;
		}

		DWORD written = 0;
		WriteFile(file, out.data(), (DWORD)out.size(), &written, NULL);
		CloseHandle(file);

		_MESSAGE("dump: %u classes, %u functions (%u outside TESV.exe), %u KB written to %s",
				 classes, functions, external, (unsigned)(written / 1024), path);
	}

	// ======================= passo 5: sonda do esqueleto do jogador =======================

	bool Readable(unsigned address, unsigned size)
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

	unsigned Rd32(unsigned a)	{ return Readable(a, 4) ? *(const unsigned *)a : 0; }
	unsigned Rd16(unsigned a)	{ return Readable(a, 2) ? *(const unsigned short *)a : 0; }
	float    RdF (unsigned a)	{ return Readable(a, 4) ? *(const float *)a : 0.0f; }

	// Nome da classe do objeto em 'obj' (a partir da vtable e do RTTI), ou "?".
	void ClassOf(const std::vector<vt::Region> & regions, unsigned lo, unsigned hi, unsigned obj, char * out, size_t size)
	{
		out[0] = '?'; out[1] = 0;
		const unsigned vtable = Rd32(obj);
		if(!vtable) return;

		vt::Named n;
		if(vt::Describe(regions, lo, hi, vtable, n))
		{
			strncpy_s(out, size, n.name, _TRUNCATE);
		}
	}

	// Texto ASCII imprimivel em 'address' (pontos para o resto).
	void AsciiAt(unsigned address, char * out, size_t size)
	{
		out[0] = 0;
		if(!address || !Readable(address, (unsigned)(size - 1))) return;

		size_t i = 0;
		for(; i + 1 < size; ++i)
		{
			const unsigned char ch = *(const unsigned char *)(address + i);
			if(ch == 0) break;
			out[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
		}
		out[i] = 0;
	}

	// Maior desvio de uma matriz 3x3 (9 floats em 'a') em relacao a uma matriz de rotacao.
	float RotationError(unsigned a)
	{
		float m[9];
		for(int i = 0; i < 9; ++i) m[i] = RdF(a + 4 * i);

		float worst = 0.0f;
		for(int i = 0; i < 3; ++i)
			for(int j = 0; j < 3; ++j)
			{
				float dot = 0.0f;
				for(int k = 0; k < 3; ++k) dot += m[i * 3 + k] * m[j * 3 + k];	// linha i . linha j
				float err = dot - (i == j ? 1.0f : 0.0f);
				if(err < 0) err = -err;
				if(err > worst) worst = err;
			}
		return worst;
	}

	void PrintNode(const std::vector<vt::Region> & regions, unsigned lo, unsigned hi,
				   unsigned node, int depth, unsigned & budget)
	{
		if(budget == 0 || depth > 9 || !Readable(node, 0xC0)) return;
		--budget;

		char cls[112];	ClassOf(regions, lo, hi, node, cls, sizeof(cls));
		const unsigned namePtr = Rd32(node + 0x08);
		char n8[40], nC[40];
		AsciiAt(namePtr + 0x08, n8, sizeof(n8));
		AsciiAt(namePtr + 0x0C, nC, sizeof(nC));

		char indent[32];
		int pad = depth * 2; if(pad > 20) pad = 20;
		memset(indent, ' ', pad); indent[pad] = 0;

		_MESSAGE("scene: %s%08X %-24s name[+8]='%s' name[+C]='%s' parent=%08X flags=%08X radius=%.1f",
				 indent, node, cls, n8, nC, Rd32(node + 0x1C), Rd32(node + 0x98), RdF(node + 0x94));
		_MESSAGE("scene: %s   local t=(%.2f %.2f %.2f) s=%.3f | world t=(%.2f %.2f %.2f) s=%.3f",
				 indent, RdF(node + 0x44), RdF(node + 0x48), RdF(node + 0x4C), RdF(node + 0x50),
				 RdF(node + 0x78), RdF(node + 0x7C), RdF(node + 0x80), RdF(node + 0x84));

		// so os nos (NiNode e derivados) tem a lista de filhos nestes deslocamentos
		if(!strstr(cls, "Node")) return;

		const unsigned data  = Rd32(node + 0xAC);
		const unsigned count = Rd16(node + 0xB2);
		if(!data || count == 0 || count > 512 || !Readable(data, count * 4)) return;

		for(unsigned i = 0; i < count; ++i)
		{
			const unsigned child = Rd32(data + 4 * i);
			if(child) PrintNode(regions, lo, hi, child, depth + 1, budget);
		}
	}

	void ProbeScene()
	{
		std::vector<vt::Region> regions;
		unsigned codeLo = 0, codeHi = 0;
		if(!Collect(regions, codeLo, codeHi)) { _MESSAGE("scene: could not read the executable's sections"); return; }

		const unsigned player = Rd32(0x01310588);	// g_thePlayer (SKSE LE), o mesmo que o Freerun LE usa
		if(!player) { _MESSAGE("scene: no player object yet"); return; }

		// TESObjectREFR::GetNiRootNode(firstPerson) = slot 0x6F (como no Freerun LE)
		const unsigned vtable = Rd32(player);
		if(!Readable(vtable, (0x6F + 1) * 4)) { _MESSAGE("scene: player vtable unreadable"); return; }

		typedef unsigned (__thiscall * GetRoot_t)(void *, unsigned);
		const GetRoot_t getRoot = (GetRoot_t)Rd32(vtable + 0x6F * 4);

		const unsigned third = getRoot((void *)player, 0);
		const unsigned first = getRoot((void *)player, 1);
		_MESSAGE("scene: player=%08X  third-person root=%08X  first-person root=%08X", player, third, first);
		if(!third) { _MESSAGE("scene: the player has no 3D yet"); return; }

		char cls[112];
		ClassOf(regions, codeLo, codeHi, third, cls, sizeof(cls));
		_MESSAGE("scene: third-person root class = %s", cls);
		ClassOf(regions, codeLo, codeHi, first, cls, sizeof(cls));
		_MESSAGE("scene: first-person root class = %s", first ? cls : "(none)");

		// A) campos do no raiz que apontam para objetos com RTTI
		_MESSAGE("scene: --- A) pointer fields inside the third-person root (offset -> class) ---");
		for(unsigned off = 0; off < 0x1C0; off += 4)
		{
			const unsigned v = Rd32(third + off);
			if(v < 0x10000 || !Readable(v, 4)) continue;
			ClassOf(regions, codeLo, codeHi, v, cls, sizeof(cls));
			if(cls[0] != '?') _MESSAGE("scene:   +0x%03X -> %08X  %s", off, v, cls);
		}

		// B) a matriz de rotacao e a escala sao validas nos deslocamentos deduzidos?
		_MESSAGE("scene: --- B) layout check on the root ---");
		_MESSAGE("scene:   local rotation error (0 = perfect rotation) = %.5f   scale=%.4f", RotationError(third + 0x20), RdF(third + 0x50));
		_MESSAGE("scene:   world rotation error                         = %.5f   scale=%.4f", RotationError(third + 0x54), RdF(third + 0x84));

		// C) bytes brutos do inicio do no (para eu confirmar os campos)
		_MESSAGE("scene: --- C) first 0xC0 bytes of the root ---");
		for(unsigned row = 0; row < 0xC0; row += 16)
			_MESSAGE("scene:   +%03X  %08X %08X %08X %08X", row,
					 Rd32(third + row), Rd32(third + row + 4), Rd32(third + row + 8), Rd32(third + row + 12));

		// D) a arvore de nos
		_MESSAGE("scene: --- D) node tree ---");
		unsigned budget = 150;
		PrintNode(regions, codeLo, codeHi, third, 0, budget);
		_MESSAGE("scene: --- end (%u nodes printed) ---", 150 - budget);
	}

	void ScanAndLog()
	{
		const DWORD t0 = GetTickCount();

		std::vector<vt::Region> regions;
		unsigned codeLo = 0, codeHi = 0;
		if(!Collect(regions, codeLo, codeHi))
		{
			_MESSAGE("rtti: could not read the executable's sections");
			return;
		}

		unsigned totalKB = 0;
		for(size_t i = 0; i < regions.size(); ++i) totalKB += regions[i].size / 1024;
		_MESSAGE("rtti: scanning %u KB of data; code range %08X-%08X", totalKB, codeLo, codeHi);

		unsigned checked = 0, same = 0, notFound = 0;

		for(size_t w = 0; w < sizeof(kWanted) / sizeof(kWanted[0]); ++w)
		{
			const Wanted & want = kWanted[w];
			vt::Result r = vt::Find(regions, codeLo, codeHi, want.mangled);

			if(!r.typeFound)
			{
				// Tipos declarados como "struct" levam .?AU em vez de .?AV.
				char alt[160];
				_snprintf_s(alt, sizeof(alt), _TRUNCATE, "%s", want.mangled);
				if(alt[0] == '.' && alt[1] == '?' && alt[2] == 'A' && alt[3] == 'V')
				{
					alt[3] = 'U';
					r = vt::Find(regions, codeLo, codeHi, alt);
					if(r.typeFound) _MESSAGE("rtti: %s is declared as a struct (.?AU)", want.label);
				}
			}

			if(!r.typeFound || r.vtables.empty())
			{
				++notFound;
				_MESSAGE("rtti: %-26s %s", want.label, r.typeFound ? "type found, but no vtable" : "NOT FOUND");
				continue;
			}

			bool match = false;
			for(size_t v = 0; v < r.vtables.size(); ++v)
			{
				const vt::Found & f = r.vtables[v];
				if(f.vtable == want.known) match = true;

				_MESSAGE("rtti: %-26s vtable %08X  slots=%-3u offset=%d", want.label, f.vtable, f.slots, f.offset);
			}

			if(want.known)
			{
				++checked;
				if(match) ++same;
				_MESSAGE("rtti:   cross-check with Freerun LE (%08X): %s", want.known, match ? "SAME" : "DIFFERENT");
			}
		}

		_MESSAGE("rtti: done in %u ms - cross-check %u of %u same; %u class(es) not found",
				 (unsigned)(GetTickCount() - t0), same, checked, notFound);

		DiscoverNames(regions, codeLo, codeHi);
		DumpVtablesToFile(regions, codeLo, codeHi);
	}
}

void ReportRtti()
{
	__try
	{
		ScanAndLog();
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		_MESSAGE("rtti: the scan faulted (code %08X) and was abandoned", (unsigned)GetExceptionCode());
	}
}

namespace
{
	void SafeProbe()
	{
		__try
		{
			ProbeScene();
		}
		__except(EXCEPTION_EXECUTE_HANDLER)
		{
			_MESSAGE("scene: the probe faulted (code %08X) and was abandoned", (unsigned)GetExceptionCode());
		}
	}

	DWORD WINAPI RetryThread(LPVOID)
	{
		Sleep(12000);	// da tempo ao jogo para carregar o personagem
		_MESSAGE("scene: retrying after 12 s");
		SafeProbe();
		return 0;
	}
}

void ReportScene()
{
	static int calls = 0;
	if(++calls > 2) return;		// so as duas primeiras cargas de save

	_MESSAGE("scene: probing the player's skeleton (read-only)...");
	SafeProbe();

	// o personagem pode ainda nao ter 3D logo apos a mensagem: repete uma vez mais tarde
	HANDLE t = CreateThread(NULL, 0, RetryThread, NULL, 0, NULL);
	if(t) CloseHandle(t);
}
