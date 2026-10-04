// SceneHooks.h - passo 8: interceptadores de PASSAGEM nas funcoes de atualizacao da cena.
// Nao alteram nada: contam as chamadas e chamam a funcao original com os mesmos argumentos.
#pragma once

// Instala os 16 interceptadores (vtables de NiNode, BSFadeNode, BSFlattenedBoneTree, NiAVObject).
void InstallSceneHooks();

// Diz quais sao o no raiz e a tabela achatada do jogador (para contar so as chamadas dele).
void SceneHooksSetTarget(unsigned root, unsigned flat);

// Arranca uma thread que, durante ~40 s, regista no log quantas chamadas houve por segundo.
void SceneHooksStartReport();

// Passo 12: experiencia de escrita AUTOMATICA e MEDIDA. Sem teclas: 8 s depois de a sonda
// terminar, o osso do braco esquerdo (NPC L UpperArm) descreve um movimento circular amplo
// durante 6 s, pára 6 s, e repete 6 vezes. A rotacao e escrita na entrada da tabela achatada
// imediatamente antes da atualizacao e restaurada logo a seguir. O plugin MEDE no log quanto
// a rotacao mundial do osso mudou, para se saber, sem depender do olho, se o jogo usou a escrita.
void SceneHooksStartExperiment(unsigned headIndex, unsigned armIndex);
