#pragma once

// Rows: namespace, symbol, numeric key, PC, display name, visibility, owner.
// Numeric keys, PCs, names, and stable IDs are persistent compatibility data.

// Overworld breakpoints
#define BP_TABLE_OVERWORLD(X) \
  X(overworld, OverworldInit,      501, 0x00000000u, "OverworldInit",     PlayerVisible, Shared) \
  X(overworld, StartTravelInputs,  502, 0x00000000u, "StartTravelInputs", PlayerVisible, Shared) \
  X(overworld, RandomEncounter,    503, 0x00000000u, "RandomEncounter",   PlayerVisible, Shared) \
  X(overworld, ReachedGoal,        504, 0x00000000u, "ReachedGoal",       PlayerVisible, Shared)

// Pre-battle / RNG probe breakpoints
#define BP_TABLE_PREBATTLE(X) \
  X(prebattle, BeforeRandSeedSet,  101, 0x80101e48u, "BeforeRandSeedSet", PlayerVisible, Shared) \
  X(prebattle, AfterRandSeedSet,   102, 0x8000a1dcu, "AfterRandSeedSet",  PlayerVisible, Shared)

// Battle breakpoints
#define BP_TABLE_BATTLE(X) \
  X(battle, BattleInit,            201, 0x8000a1dcu, "BattleInit",         PlayerVisible, Shared) \
  X(battle, BattleInitComplete,    202, 0x8000a2d4u, "BattleInitComplete", PlayerVisible, Shared) \
  X(battle, TurnInputs,            203, 0x80071740u, "TurnInputs",         PlayerVisible, Shared) \
  X(battle, TurnIsReady,           204, 0x800715ecu, "TurnIsReady",        PlayerVisible, Shared)  /* This should be at the end of the turn init fxn which generates enemy instructions and turn order. */\
  X(battle, StartTurn,             205, 0x800715dcu, "StartTurn",          PlayerVisible, Shared) \
  X(battle, StartAction,           206, 0x800715dcu, "StartAction",        PlayerVisible, Shared) \
  X(battle, EndAction,             207, 0x8007050cu, "EndAction",          PlayerVisible, Shared) \
  X(battle, EndTurn,               208, 0x800702a0u, "EndTurn",            PlayerVisible, Shared) \
  X(battle, EndBattleVictory,      209, 0x800706d8u, "Battle_Victory",     PlayerVisible, Shared) \
  X(battle, EndBattleDefeat,       210, 0x8007066cu, "Battle_Defeat",      PlayerVisible, Shared) \
  X(battle, BattleLoadComplete,    211, 0x800307a0u, "BattleLoadComplete", PlayerVisible, Shared) \
  X(battle, BattleMacroMainMenuMoveHigher,    220, 0x8007cfd8u, "BattleMacroMainMenuMoveHigher",    Internal, InputMacro) \
  X(battle, BattleMacroMainMenuMoveLower,     221, 0x8007d020u, "BattleMacroMainMenuMoveLower",     Internal, InputMacro) \
  X(battle, BattleMacroMainMenuMoveHigherAlt, 222, 0x8007d06cu, "BattleMacroMainMenuMoveHigherAlt", Internal, InputMacro) \
  X(battle, BattleMacroMainMenuMoveLowerAlt,  223, 0x8007d0b4u, "BattleMacroMainMenuMoveLowerAlt",  Internal, InputMacro) \
  X(battle, BattleMacroMainMenuAcceptDispatch, 224, 0x8007d0e0u, "BattleMacroMainMenuAcceptDispatch", Internal, InputMacro) \
  X(battle, BattleMacroDirectCommandQueued,   225, 0x8007c7e4u, "BattleMacroDirectCommandQueued",   Internal, InputMacro) \
  X(battle, BattleMacroAttackTargetSelectorCreated, 226, 0x8007c73cu, "BattleMacroAttackTargetSelectorCreated", Internal, InputMacro) \
  X(battle, BattleMacroEnemyTargetCursorMoved, 227, 0x800797a8u, "BattleMacroEnemyTargetCursorMoved", Internal, InputMacro) \
  X(battle, BattleMacroEnemyTargetWritten,    228, 0x800798b8u, "BattleMacroEnemyTargetWritten",    Internal, InputMacro) \
  X(battle, BattleMacroEnemyTargetFinalized,  229, 0x800798c0u, "BattleMacroEnemyTargetFinalized",  Internal, InputMacro) \
  X(battle, BattleMacroEnemyTargetMoveDownAccepted, 240, 0x8007976cu, "BattleMacroEnemyTargetMoveDownAccepted", Internal, InputMacro) \
  X(battle, BattleMacroEnemyTargetMoveUpAccepted,   241, 0x8007973cu, "BattleMacroEnemyTargetMoveUpAccepted",   Internal, InputMacro) \
  X(battle, BattleMacroCommandTransitionDone, 230, 0x80026238u, "BattleMacroCommandTransitionDone", Internal, InputMacro) \
  X(battle, BattleMacroInputReadyGate,        231, 0x8007cec4u, "BattleMacroInputReadyGate",        Internal, InputMacro) \
  X(battle, BattleMacroMagicReady,            232, 0x8007b65cu, "BattleMacroMagicReady",            Internal, InputMacro) \
  X(battle, BattleMacroSMoveReady,            233, 0x8007bf78u, "BattleMacroSMoveReady",            Internal, InputMacro) \
  X(battle, BattleMacroConditionalRunReady,   234, 0x8007c3e4u, "BattleMacroConditionalRunReady",   Internal, InputMacro) \
  X(battle, BattleMacroItemCategoryReady,     235, 0x8007a94cu, "BattleMacroItemCategoryReady",     Internal, InputMacro) \
  X(battle, BattleMacroItemRowListReady,      236, 0x8007ab50u, "BattleMacroItemRowListReady",      Internal, InputMacro) \
  X(battle, BattleMacroItemDetailReady,       237, 0x80079d04u, "BattleMacroItemDetailReady",       Internal, InputMacro) \
  X(battle, BattleMacroEnemyTargetReady,      238, 0x800796f0u, "BattleMacroEnemyTargetReady",      Internal, InputMacro) \
  X(battle, BattleMacroAllyTargetReady,       239, 0x80078fd8u, "BattleMacroAllyTargetReady",       Internal, InputMacro) \

#define BP_TABLE_ALL(X) \
  BP_TABLE_OVERWORLD(X) \
  BP_TABLE_PREBATTLE(X) \
  BP_TABLE_BATTLE(X)
