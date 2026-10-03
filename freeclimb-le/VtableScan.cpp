#include "VtableScan.h"

#include <string.h>

namespace vt
{
	namespace
	{
		bool Read32(const std::vector<Region> & regions, unsigned address, unsigned & out)
		{
			for(size_t i = 0; i < regions.size(); ++i)
			{
				const Region & r = regions[i];
				if(address >= r.address && r.size >= 4 && address - r.address <= r.size - 4)
				{
					memcpy(&out, r.data + (address - r.address), 4);
					return true;
				}
			}
			return false;
		}

		bool InCode(unsigned value, unsigned lo, unsigned hi)
		{
			return value >= lo && value < hi;
		}

		// Procura o nome decorado (com o \0 final) e devolve o endereco do TypeDescriptor.
		bool FindTypeDescriptor(const std::vector<Region> & regions, const char * name, unsigned & td)
		{
			const size_t len = strlen(name) + 1;	// inclui o \0
			for(size_t i = 0; i < regions.size(); ++i)
			{
				const Region & r = regions[i];
				if(r.size < len + 8) continue;

				for(unsigned off = 8; off + len <= r.size; ++off)
				{
					if(r.data[off] != (unsigned char)name[0]) continue;
					if(memcmp(r.data + off, name, len) != 0) continue;

					td = r.address + off - 8;
					return true;
				}
			}
			return false;
		}
	}

	Result Find(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi, const char * mangledName)
	{
		Result result;
		result.typeFound = false;
		result.typeDescriptor = 0;

		if(!FindTypeDescriptor(regions, mangledName, result.typeDescriptor))
			return result;

		result.typeFound = true;

		// 1) todos os COL que apontam para este TypeDescriptor
		std::vector<unsigned> cols;
		for(size_t i = 0; i < regions.size(); ++i)
		{
			const Region & r = regions[i];
			for(unsigned off = 12; off + 4 <= r.size; off += 4)
			{
				if(((r.address + off) & 3) != 0) continue;

				unsigned value;
				memcpy(&value, r.data + off, 4);
				if(value != result.typeDescriptor) continue;

				unsigned signature = 0;
				memcpy(&signature, r.data + off - 12, 4);
				if(signature != 0) continue;	// COL de 32 bits tem assinatura 0

				cols.push_back(r.address + off - 12);
			}
		}

		// 2) para cada COL, a vtable que o referencia (o dword antes do slot 0)
		for(size_t c = 0; c < cols.size(); ++c)
		{
			for(size_t i = 0; i < regions.size(); ++i)
			{
				const Region & r = regions[i];
				for(unsigned off = 0; off + 8 <= r.size; off += 4)
				{
					if(((r.address + off) & 3) != 0) continue;

					unsigned value;
					memcpy(&value, r.data + off, 4);
					if(value != cols[c]) continue;

					const unsigned vtable = r.address + off + 4;
					unsigned first = 0;
					if(!Read32(regions, vtable, first) || !InCode(first, codeLo, codeHi)) continue;

					Found f;
					f.colAddress = cols[c];
					f.offset = 0;
					unsigned offsetField = 0;
					if(Read32(regions, cols[c] + 4, offsetField)) f.offset = (int)offsetField;
					f.vtable = vtable;
					f.slots = 0;

					unsigned slot;
					while(f.slots < 1024 && Read32(regions, vtable + f.slots * 4, slot) && InCode(slot, codeLo, codeHi))
						++f.slots;

					result.vtables.push_back(f);
				}
			}
		}

		return result;
	}
}
