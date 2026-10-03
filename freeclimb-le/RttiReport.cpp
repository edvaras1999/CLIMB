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
