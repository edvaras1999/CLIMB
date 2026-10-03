// PackLoader.h - a unica ligacao entre o plugin (SKSE, C++ antigo) e o nucleo do
// FreeClimb (C++20). So tipos simples: nada de std::filesystem nem de cabecalhos do SKSE.
#pragma once

struct PackSummary
{
	bool		committed;
	unsigned	loaded, rejected, missing;
	unsigned	inputKB, outputKB;
	unsigned	milliseconds;
	char		error[256];
	char		problems[1024];	// "ficheiro: motivo; ..." para o que nao carregou
};

// Carrega o pacote de animacoes (pack.json + configs + .hkx). Nunca lanca excecoes.
bool FC_LoadAnimationPack(const char * manifestPath, PackSummary * out);
