#pragma once
#include "Core/Memory/Soa/Battle/BattleContext.h"

struct BattleContextTree {
	static void DrawTree(soa::battle::ctx::BattleContext bc);
	static void DrawLines(soa::battle::ctx::BattleContext bc);
};