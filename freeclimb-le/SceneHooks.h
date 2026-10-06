// SceneHooks.h - passo 8: interceptadores de PASSAGEM nas funcoes de atualizacao da cena.
// Nao alteram nada: contam as chamadas e chamam a funcao original com os mesmos argumentos.
#pragma once

// Instala os 16 interceptadores (vtables de NiNode, BSFadeNode, BSFlattenedBoneTree, NiAVObject).
void InstallSceneHooks();

// Diz quais sao o no raiz e a tabela achatada do jogador (para contar so as chamadas dele).
void SceneHooksSetTarget(unsigned root, unsigned flat);

// Arranca uma thread que, durante ~40 s, regista no log quantas chamadas houve por segundo.
void SceneHooksStartReport();

// Passo 17: aplica ao esqueleto do jogador poses REAIS do pacote de animacoes do FreeClimb.
// 'entryIndex' tem 99 posicoes: o indice da entrada da tabela achatada de cada osso canonico (-1 = nenhum).
void SceneHooksBindBones(const int * entryIndex);

// Depois de o jogador andar e parar, verifica a matematica e aplica 5 poses, uma de cada vez (6 s cada).
void SceneHooksStartPoseTest();
