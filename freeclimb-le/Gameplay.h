// Gameplay.h - passo 19: o primeiro comportamento real do mod.
// Carregar em FURTIVIDADE + SALTAR junto a uma borda alcancavel: o plugin deteta a borda com raios do Havok,
// toca a animacao de subida (contextMantle) no corpo do personagem e, no fim, coloca-o em cima da borda.
#pragma once

void InstallGameplay();
