// RttiReport.h - passo 3: descobrir, com o jogo a correr, onde ficam as vtables
// (tabelas de funcoes virtuais) das classes de que o FreeClimb precisa.
#pragma once

// Escreve no log o endereco e o tamanho da vtable de cada classe da lista.
// Nunca fecha o jogo: qualquer falha de leitura de memoria e apanhada e registada.
void ReportRtti();
