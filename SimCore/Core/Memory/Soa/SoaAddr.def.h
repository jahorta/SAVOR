#pragma once

// domain, NAME,                  REGION, MODE,     BASE_VA,     WIDTH, /* optional offsets... */
// Offsets semantics for PtrChain: cur=BASE; for each off: cur = *(u32*)cur + off; final VA = cur.

#define ADDR_TABLE_CORE(X) \
  X(core,   RNG_SEED,               MEM1,      0x803469A8) \
  X(core,   SCT_FILE_NUM,           MEM1,      0x80311AC4) \
  X(core,   SCT_FILE_LTTR,          MEM1,      0x80311AC8) \
  X(core,   SCT_INDEX_BUFFER,       MEM1,      0x8030D6C0) \
  X(core,   SCT_SECTION_COUNT,      MEM1,      0x8030D6C4) \
  X(core,   SCT_FIRST_INST,         MEM1,      0x8030CEA0) \
  X(core,   SCT_CURRENT_INST,       MEM1,      0x8030CEA4)

#define ADDR_TABLE_BATTLE(X) \
  X(battle, CombatantInstancesTable,     MEM1,      0x80309DE4) /* 12 x u32 pointers (PC0..PC3, EC0..EC7) */\
  X(battle, CombatantIdTable,            MEM1,      0x80309DCC) /* 12 x u8 IDs parallel to instances */\
  X(battle, TurnOrderTable,              MEM1,      0x803092f4) /* 12 x u8 IDs in turn order */\
  X(battle, MainInstancePtr,             MEM1,      0x80347390) /* Singular instance for recording overall battle details */\
  X(battle, BattlePhase,                 MEM1,      0x8034737c) /* Current Battle phase */\
  X(battle, TurnType,                    MEM1,      0x80347344) /* Turn type for detecting a back attack */\
  X(battle, TurnPhase,                   MEM1,      0x8034733c) /* Current Turn phase */\
  X(battle, CurrentTurn,                 MEM1,      0x80347340) /* Current Turn number */\
  X(battle, Instructions,                MEM1,      0x80309174) /* Combatant Instruction Sheet 12 x QueuedInstructionWorksheet (size=0x20) */\


/*
  Example PtrChain: EnemyDefinition pointer at +0x110 from a CombatantInstance BASE. 
     BASE is dynamic (the instance VA); pass it via resolve_from_base(). 
  X(battle, EnemyDefinitionFromInstance, MEM1, 0x00000000)
  
  X(battle, MainInstanceCurrentSP,       MEM1, PtrChain, 0x80347390, U16, 0x8) \
  X(battle, MainInstanceMaxSP,           MEM1, PtrChain, 0x80347390, U16, 0x6) \
  X(battle, MainInstanceInitiative,      MEM1, PtrChain, 0x80347390, U32, 0x0) \
  X(battle, MainInstanceTreasureTable,   MEM1, PtrChain, 0x80347390, U32, 0xE) \
  X(battle, MainInstanceInventory,       MEM1, PtrChain, 0x80347390, U32, 0x38)
*/