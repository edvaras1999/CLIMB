// PackLoader.cpp - compilado em C++20, sem cabecalhos do SKSE nem do Windows.
#include "PackLoader.h"
#include "src/AnimationPack.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <string>

namespace
{
	void CopyText(char * dest, size_t size, const std::string & text)
	{
		if(size == 0) return;
		const size_t n = text.size() < size - 1 ? text.size() : size - 1;
		std::memcpy(dest, text.data(), n);
		dest[n] = '\0';
	}
}

bool FC_LoadAnimationPack(const char * manifestPath, PackSummary * out)
{
	if(!out) return false;
	std::memset(out, 0, sizeof(*out));

	try
	{
		// Criada na primeira chamada e nunca destruida: evita ordem de destruicao
		// de globais quando o jogo descarrega a DLL.
		static fc::Library * library = new fc::Library;

		const auto start = std::chrono::steady_clock::now();
		const fc::AnimationPackReport report = fc::loadAnimationPack(*library, manifestPath);
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

		out->committed		= report.committed;
		out->loaded			= (unsigned)report.loaded;
		out->rejected		= (unsigned)report.rejected;
		out->missing		= (unsigned)report.missing;
		out->inputKB		= (unsigned)(report.inputBytes / 1024);
		out->outputKB		= (unsigned)(report.outputBytes / 1024);
		out->milliseconds	= (unsigned)elapsed.count();
		CopyText(out->error, sizeof(out->error), report.error);

		std::string problems;
		for(const auto & slot : report.slots)
		{
			if(slot.status == fc::OverrideStatus::loaded) continue;
			if(problems.size() > 800) { problems += "..."; break; }
			problems += slot.file + ": " + slot.reason + "; ";
		}
		CopyText(out->problems, sizeof(out->problems), problems);
		return report.committed;
	}
	catch(const std::exception & e)
	{
		CopyText(out->error, sizeof(out->error), std::string("exception: ") + e.what());
	}
	catch(...)
	{
		CopyText(out->error, sizeof(out->error), "unknown exception");
	}
	return false;
}
