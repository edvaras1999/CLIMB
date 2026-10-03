// VtableScan.h - encontra a "vtable" (tabela de funcoes virtuais) de uma classe C++
// compilada com o Visual C++, a partir do nome da classe, usando a informacao de
// RTTI que o compilador deixa no executavel. Codigo puro (sem SKSE nem Windows),
// para poder ser testado fora do jogo.
//
// Estrutura do RTTI do MSVC em x86:
//   TypeDescriptor : [ptr vftable][reservado][nome decorado, ex. ".?AVJumpHandler@@\0"]
//   CompleteObjectLocator (COL): [0][deslocamento][cdOffset][ptr TypeDescriptor][ptr ClassHierarchy]
//   vtable : o dword ANTES do primeiro slot aponta para o COL.
#pragma once
#include <vector>

namespace vt
{
	// Um bloco de memoria legivel: 'data' guarda os bytes que estao no endereco 'address'.
	struct Region
	{
		unsigned				address;
		unsigned				size;
		const unsigned char *	data;
	};

	struct Found
	{
		unsigned	colAddress;	// endereco do COL
		int			offset;		// 0 = vtable principal; != 0 = vtable de uma base (heranca multipla)
		unsigned	vtable;		// endereco do slot 0
		unsigned	slots;		// quantos slots seguidos apontam para codigo
	};

	struct Result
	{
		bool				typeFound;
		unsigned			typeDescriptor;
		std::vector<Found>	vtables;
	};

	// 'codeLo'..'codeHi' e o intervalo de enderecos executaveis (.text).
	Result Find(const std::vector<Region> & regions, unsigned codeLo, unsigned codeHi, const char * mangledName);
}
