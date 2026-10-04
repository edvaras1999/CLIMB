// SceneHooks.h - passo 8: interceptadores de PASSAGEM nas funcoes de atualizacao da cena.
// Nao alteram nada: contam as chamadas e chamam a funcao original com os mesmos argumentos.
#pragma once

// Instala os 16 interceptadores (vtables de NiNode, BSFadeNode, BSFlattenedBoneTree, NiAVObject).
void InstallSceneHooks();

// Diz quais sao o no raiz e a tabela achatada do jogador (para contar so as chamadas dele).
void SceneHooksSetTarget(unsigned root, unsigned flat);

// Arranca uma thread que, durante ~40 s, regista no log quantas chamadas houve por segundo.
void SceneHooksStartReport();

// Passo 11: experiencia de escrita CONTROLADA POR TECLA. O plugin so escreve quando o
// utilizador carrega em F7 (cabeca) ou F8 (braco esquerdo): durante 10 s o osso descreve um
// movimento circular amplo, escrito na entrada da tabela achatada imediatamente antes da
// atualizacao do esqueleto e restaurado logo a seguir. Sem tecla, nao escreve nada.
// Os indices sao os das entradas da tabela achatada (0xFFFFFFFF = osso nao encontrado).
void SceneHooksStartExperiment(unsigned headIndex, unsigned armIndex);
