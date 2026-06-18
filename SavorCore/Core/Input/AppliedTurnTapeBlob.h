#pragma once
#include "BattleInputTraceBlob.h"

namespace savor::inputtape {

    using TurnChunk = savor::inputtrace::BattleTurnInputTrace;

    using savor::inputtrace::durations_csv;
    using savor::inputtrace::ensure_header;
    using savor::inputtrace::read_u32;
    using savor::inputtrace::render_text;
    using savor::inputtrace::turn_windows_csv;
    using savor::inputtrace::write_u32;

    inline bool append_turn_chunk(std::string& blob, const TurnChunk& chunk) {
        return savor::inputtrace::append_turn_input_trace(blob, chunk);
    }

    inline bool decode_turn_chunks(const std::string& blob, std::vector<TurnChunk>& out) {
        return savor::inputtrace::decode_turn_input_traces(blob, out);
    }

    inline savor::InputPlan flatten_plan(const std::vector<TurnChunk>& chunks) {
        return savor::inputtrace::flatten_controller_input_sequence(chunks);
    }
}
