
/* DERIVED Battle buffer layout (offsets are BASE_VA of the derived battle region)

Header (32 bytes, aligned)
0x0000 : HeaderMagic          (U32)   // 'DBUF' = 0x44425546
0x0004 : HeaderVersion        (U16)   // 1
0x0006 : HeaderMaxItemId      (U16)   // 512
0x0008 : HeaderLastUpdateBp   (U16)
0x000A : HeaderLastUpdateTurn (U16)
0x000C : HeaderFlags          (U32)   // bit0=valid; others reserved
0x0010 : HeaderReserved[16]   (bytes) // pad to 0x20

Existing fields (shifted by +0x20)
0x0020         : CurrentTurn (U32)
0x0024         : TurnOrderSize (U32)
0x0028..0x0033 : TurnOrderIdx_[PC0..PC3, Enemy0..Enemy7] (U8 each, 12 total)
0x0034         : TurnOrderPcMin (U8)
0x0035         : TurnOrderPcMax (U8)
0x0036         : TurnOrderEcMin (U8)
0x0037         : TurnOrderEcMax (U8)
0x0038..0x003F : reserved/pad

New dense item tables (aligned)
0x0040..0x023F : DropsByItem[512]     (U8 each; clamp [0,99])
0x0240..0x043F : InventoryByItem[512] (U8 each; clamp [0,99])

Total footprint so far: 0x0440 (1088 bytes)
*/

#define ADDR_TABLE_DERIVED_BATTLE(X) \
  \
  /* Header */ \
  X(derived_battle, HeaderMagic,            DERIVED, 0x0000) \
  X(derived_battle, HeaderVersion,          DERIVED, 0x0004) \
  X(derived_battle, HeaderMaxItemId,        DERIVED, 0x0006) \
  X(derived_battle, HeaderLastUpdateBp,     DERIVED, 0x0008) \
  X(derived_battle, HeaderLastUpdateTurn,   DERIVED, 0x000A) \
  X(derived_battle, HeaderFlags,            DERIVED, 0x000C) \
  \
  /* Existing fields (shifted by +0x20) */ \
  X(derived_battle, CurrentTurn,            DERIVED, 0x0020) \
  X(derived_battle, TurnOrderSize,          DERIVED, 0x0024) \
  X(derived_battle, TurnOrderIdx_base,      DERIVED, 0x0028) \
  X(derived_battle, TurnOrderPcMin,         DERIVED, 0x0034) \
  X(derived_battle, TurnOrderPcMax,         DERIVED, 0x0035) \
  X(derived_battle, TurnOrderEcMin,         DERIVED, 0x0036) \
  X(derived_battle, TurnOrderEcMax,         DERIVED, 0x0037) \
  \
  /* New dense item tables */ \
  X(derived_battle, DropsByItem_base,       DERIVED, 0x0040) \
  X(derived_battle, InventoryByItem_base,   DERIVED, 0x0240) \
  \
  /* Next entry should be at 0x0440 */ \
