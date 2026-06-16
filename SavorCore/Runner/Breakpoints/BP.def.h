#pragma once

// Overworld breakpoints
#define BP_TABLE_OVERWORLD(X) \
  X(overworld, OverworldInit,      501, 0x00000000u, "OverworldInit") \
  X(overworld, StartTravelInputs,  502, 0x00000000u, "StartTravelInputs") \
  X(overworld, RandomEncounter,    503, 0x00000000u, "RandomEncounter") \
  X(overworld, ReachedGoal,        504, 0x00000000u, "ReachedGoal")

// Pre-battle / RNG probe breakpoints
#define BP_TABLE_PREBATTLE(X) \
  X(prebattle, BeforeRandSeedSet,  101, 0x80101e48u, "BeforeRandSeedSet") \
  X(prebattle, AfterRandSeedSet,   102, 0x8000a1dcu, "AfterRandSeedSet")

// Battle breakpoints
#define BP_TABLE_BATTLE(X) \
  X(battle, BattleInit,            201, 0x8000a1dcu, "BattleInit") \
  X(battle, BattleInitComplete,    202, 0x8000a2d4u, "BattleInitComplete") \
  X(battle, TurnInputs,            203, 0x80071740u, "TurnInputs") \
  X(battle, TurnIsReady,           204, 0x800715ecu, "TurnIsReady")  /* This should be at the end of the turn init fxn which generates enemy instructions and turn order. */\
  X(battle, StartTurn,             205, 0x800715dcu, "StartTurn") \
  X(battle, StartAction,           206, 0x800715dcu, "StartAction") \
  X(battle, EndAction,             207, 0x8007050cu, "EndAction") \
  X(battle, EndTurn,               208, 0x800702a0u, "EndTurn") \
  X(battle, EndBattleVictory,      209, 0x800706d8u, "Battle_Victory") \
  X(battle, EndBattleDefeat,       210, 0x8007066cu, "Battle_Defeat") \
  X(battle, BattleLoadComplete,    211, 0x800307a0u, "BattleLoadComplete") \
  X(battle, BattleMacroMainMenuMoveHigher,    220, 0x8007cfd8u, "BattleMacroMainMenuMoveHigher") \
  X(battle, BattleMacroMainMenuMoveLower,     221, 0x8007d020u, "BattleMacroMainMenuMoveLower") \
  X(battle, BattleMacroMainMenuMoveHigherAlt, 222, 0x8007d06cu, "BattleMacroMainMenuMoveHigherAlt") \
  X(battle, BattleMacroMainMenuMoveLowerAlt,  223, 0x8007d0b4u, "BattleMacroMainMenuMoveLowerAlt") \
  X(battle, BattleMacroMainMenuAcceptDispatch, 224, 0x8007d0e0u, "BattleMacroMainMenuAcceptDispatch") \
  X(battle, BattleMacroDirectCommandQueued,   225, 0x8007c7e4u, "BattleMacroDirectCommandQueued") \
  X(battle, BattleMacroAttackTargetSelectorCreated, 226, 0x8007c73cu, "BattleMacroAttackTargetSelectorCreated") \
  X(battle, BattleMacroEnemyTargetCursorMoved, 227, 0x800797a8u, "BattleMacroEnemyTargetCursorMoved") \
  X(battle, BattleMacroEnemyTargetWritten,    228, 0x800798b8u, "BattleMacroEnemyTargetWritten") \
  X(battle, BattleMacroEnemyTargetFinalized,  229, 0x800798c0u, "BattleMacroEnemyTargetFinalized") \
  X(battle, BattleMacroEnemyTargetMoveDownAccepted, 240, 0x8007976cu, "BattleMacroEnemyTargetMoveDownAccepted") \
  X(battle, BattleMacroEnemyTargetMoveUpAccepted,   241, 0x8007973cu, "BattleMacroEnemyTargetMoveUpAccepted") \
  X(battle, BattleMacroCommandTransitionDone, 230, 0x80026238u, "BattleMacroCommandTransitionDone") \
  X(battle, BattleMacroInputReadyGate,        231, 0x8007cec4u, "BattleMacroInputReadyGate") \
  X(battle, BattleMacroMagicReady,            232, 0x8007b65cu, "BattleMacroMagicReady") \
  X(battle, BattleMacroSMoveReady,            233, 0x8007bf78u, "BattleMacroSMoveReady") \
  X(battle, BattleMacroConditionalRunReady,   234, 0x8007c3e4u, "BattleMacroConditionalRunReady") \
  X(battle, BattleMacroItemCategoryReady,     235, 0x8007a94cu, "BattleMacroItemCategoryReady") \
  X(battle, BattleMacroItemRowListReady,      236, 0x8007ab50u, "BattleMacroItemRowListReady") \
  X(battle, BattleMacroItemDetailReady,       237, 0x80079d04u, "BattleMacroItemDetailReady") \
  X(battle, BattleMacroEnemyTargetReady,      238, 0x800796f0u, "BattleMacroEnemyTargetReady") \
  X(battle, BattleMacroAllyTargetReady,       239, 0x80078fd8u, "BattleMacroAllyTargetReady") \

#define BP_TABLE_ALL(X) \
  BP_TABLE_OVERWORLD(X) \
  BP_TABLE_PREBATTLE(X) \
  BP_TABLE_BATTLE(X)
