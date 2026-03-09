// SimCore/IPC/Wire.h
#pragma once
#include <cstdint>
#include "../../Core/Input/InputPlan.h"

namespace simcore {

    enum : uint32_t {
        MSG_READY = 0x01,
        MSG_JOB = 0x02,
        MSG_RESULT = 0x03,
        MSG_PROGRESS = 0x04,
        // control-mode (program lifecycle):
        MSG_SET_PROGRAM = 0x10,
        MSG_RUN_INIT_ONCE = 0x11,
        MSG_ACTIVATE_MAIN = 0x12,
        MSG_ACK = 0x13
    };

    enum : uint32_t {
        WERR_None = 0,
        WERR_SysMissing = 1,
        WERR_BootFail = 2,
        WERR_LoadGame = 3,
        WERR_VMInit = 4,
        WERR_WriteReady = 6,

        // Job errors
        WERR_NoProgramLoaded = 16,
        WERR_DecodePayloadFail = 17,
        WERR_EncodePayloadFail = 18,

        // Ctx errors
        WERR_NoGameIsoRecorded = 32,

        // Unknown
        WERR_UnknownError = 0xffffffff
    };

    enum : uint8_t {
        WSTATE_NoProgram = 0,
        WSTATE_ProgramReady = 1
    };

    enum : uint8_t {
        PK_None = 0,
        PK_SeedProbe = 1,
        PK_TasMovie = 2,
        PK_BattleTurnRunner = 3, 
        PK_BattleContextProbe = 4,
        PK_BattleSingleTurnRunner = 5,
    };

    // Payload used for TAS jobs (paths are NUL-terminated, Windows MAX_PATH safe)
    struct TasJobPayload {
        char dtm_path[260];
        char save_dir[260];
        uint32_t run_until_ms;
    };

    struct WireReady {
        uint32_t tag;   // MSG_READY
        uint8_t  ok;    // 1=ok
        uint8_t  state; // WSTATE_*
        uint16_t _pad;
        uint32_t error; // WERR_*
    };

#pragma pack(push, 1)
    struct WireResult {
        uint32_t tag;      // MSG_RESULT
        uint64_t job_id;
        uint32_t epoch;
        uint8_t  ok;       // 1=success, 0=failure (transport-level)
        uint8_t  err;
        uint8_t  _pad[2];  // keep 4-byte alignment for ctx_len
        uint32_t ctx_len;  // number of bytes that follow immediately (context blob)
    };
#pragma pack(pop)

    static_assert(sizeof(WireResult) == 24, "WireResult must be 24 bytes");

    // Phase codes (u32)
    enum : uint32_t
    {
        PHASE_UNKNOWN = 0,
        PHASE_RUN_INPUTS = 1,
        PHASE_RUN_UNTIL_BP = 2,
    };

    // Status flags (u32 bitfield)
    enum ProgressFlags : uint32_t
    {
        PF_WAITING_FOR_BP = 0x00000001,
        PF_MOVIE_PLAYING = 0x00000002,
        PF_VI_STALLED_SUSPECTED = 0x00000004,
        PF_TIMEOUT_NEAR = 0x00000008,
        PF_HEARTBEAT = 0x00000010,
    };

#pragma pack(push, 1)
    struct WireProgress
    {
        uint32_t tag;         // = MSG_PROGRESS
        uint64_t job_id;      // mirrors WireJobHeader::job_id
        bool     record_progress;
        char     text[1025];    // short hint, UTF-8, NUL-terminated if shorter
    };
#pragma pack(pop)

    struct WireSetProgram {
        uint32_t tag;         // MSG_SET_PROGRAM
        uint8_t  init_kind;   // PK_*
        uint8_t  main_kind;   // PK_*
        uint8_t  buff_kind;   // DK_* 
        uint8_t _pad0;
        uint32_t timeout_ms;
        char     savestate_path[260]; // empty => start from boot
    };

    struct WireAck {
        uint32_t tag; // MSG_ACK
        uint8_t  ok;  // 1=ok
        uint8_t  code;// 'S' (set), 'I' (init done), 'A' (main active)
        uint16_t _pad0;
    };

    struct WireJobHeader {
        uint32_t tag;      // MSG_JOB
        uint64_t job_id;
        uint32_t epoch;
        uint32_t payload_len;  // number of bytes that follow immediately
    };

} // namespace simcore
