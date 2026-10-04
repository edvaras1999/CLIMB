// SceneHooks.h - passo 8: interceptadores de PASSAGEM nas funcoes de atualizacao da cena.
// Nao alteram nada: contam as chamadas e chamam a funcao original com os mesmos argumentos.
#pragma once

// Instala os 16 interceptadores (vtables de NiNode, BSFadeNode, BSFlattenedBoneTree, NiAVObject).
void InstallSceneHooks();

// Diz quais sao o no raiz e a tabela achatada do jogador (para contar so as chamadas dele).
void SceneHooksSetTarget(unsigned root, unsigned flat);

// Arranca uma thread que, durante ~40 s, regista no log quantas chamadas houve por segundo.
void SceneHooksStartReport();
