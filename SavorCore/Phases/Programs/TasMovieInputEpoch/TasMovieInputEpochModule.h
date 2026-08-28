#pragma once


#include "Core/Input/GCInputFrame.h"
#include "Runner/Runtime/FullPhase/FullPhaseProgram.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::tasmovie::inputepoch {

inline constexpr std::uint32_t ContractVersion = 2;
inline constexpr std::int32_t ProgramVersion = 2;
inline constexpr std::string_view PadReadReturnedPointId =
    "soa.tasmovie.point.input.PadReadReturned";
inline constexpr std::uint32_t PadReadReturnedPc = 0x801D6E7Cu;
inline constexpr std::size_t MaximumEpochs = 1'000'000;

inline constexpr std::string_view AnnotationModuleCanonicalId =
    "soa.tasmovie.annotate";
inline constexpr std::string_view AnnotationEntrypoint = "annotate";
inline constexpr std::string_view AnnotationFullPhaseCanonicalId =
    "savor.full_phase.tasmovie.annotate";
inline constexpr std::string_view AnnotationBaselineLineage =
    "soa.tasmovie.input_epochs/complete-boot-dtm/v1";
inline constexpr std::string_view BreakpointDiagnosticModuleCanonicalId =
    "soa.tasmovie.input_epoch_breakpoint_diagnostic";
inline constexpr std::string_view BreakpointDiagnosticEntrypoint = "diagnose";
inline constexpr std::string_view BreakpointDiagnosticFullPhaseCanonicalId =
    "savor.full_phase.tasmovie.input_epoch_breakpoint_diagnostic";

inline constexpr std::string_view RewriteModuleCanonicalId =
    "soa.tasmovie.revise";
inline constexpr std::string_view RewriteEntrypoint = "rewrite";
inline constexpr std::string_view RewriteFullPhaseCanonicalId =
    "savor.full_phase.tasmovie.revise";
inline constexpr std::string_view RewriteBaselineLineage =
    "soa.tasmovie.input_epochs/source-playback/v1";

inline constexpr std::int32_t CutsceneProgramVersion = 1;
inline constexpr std::string_view CutsceneModuleCanonicalId =
    "soa.tasmovie.cutscene";
inline constexpr std::string_view CutsceneEntrypoint = "record";
inline constexpr std::string_view CutsceneFullPhaseCanonicalId =
    "savor.full_phase.tasmovie.cutscene";
inline constexpr std::string_view CutsceneBaselineLineage =
    "soa.tasmovie.cutscene/validated-movie-paired-checkpoint/v1";
inline constexpr std::uint32_t DialogueTextRevealInputReadyPc = 0x8010D300u;
inline constexpr std::uint32_t DialogueChoiceInputReadyPc = 0x8010CFD4u;
inline constexpr std::uint32_t PreBattleBeforeRandSeedSetPc = 0x80101E48u;
inline constexpr std::uint32_t FieldFastPreseedPc = 0x80101894u;
inline constexpr std::uint32_t FieldDeferredPreseedPc = 0x801018ACu;
inline constexpr std::uint32_t FieldFastPreseedQualificationAddress = 0x803475D4u;

enum class InputEpochOutcomeV1 : std::int64_t
{
    Completed = 0,
    Diverged = 1,
};

enum class InputEpochFailureReasonV1 : std::int64_t
{
    None = 0,
    CursorZero = 1,
    CursorRegressed = 2,
    CursorPastSource = 3,
    UnexpectedMovieEnd = 4,
    PrefixDiverged = 5,
};

struct TasMovieInputEpochV1
{
    std::uint64_t movie_input_cursor = 0;
    GCInputFrame input{};

    auto operator<=>(const TasMovieInputEpochV1&) const = default;
};

struct TasMovieInputEpochScheduleV1
{
    std::string source_dtm_sha256;
    std::uint64_t source_poll_count = 0;
    std::vector<TasMovieInputEpochV1> epochs;

    auto operator<=>(const TasMovieInputEpochScheduleV1&) const = default;
};

struct TasMovieInputEpochAnnotationRequestV1
{
    std::string source_dtm_path;
    std::string source_dtm_sha256;
    std::uint64_t source_poll_count = 0;
};

struct TasMovieInputEpochAnnotationResultV1
{
    InputEpochOutcomeV1 outcome = InputEpochOutcomeV1::Diverged;
    TasMovieInputEpochScheduleV1 schedule;
    InputEpochFailureReasonV1 failure_reason =
        InputEpochFailureReasonV1::None;
    std::uint64_t failure_epoch = 0;
    std::uint64_t expected_cursor = 0;
    std::uint64_t actual_cursor = 0;
};

struct TasMovieInputEpochRewriteRequestV1
{
    std::string source_dtm_path;
    TasMovieInputEpochScheduleV1 schedule;
    std::uint64_t insert_before_epoch = 0;
    std::uint64_t neutral_epoch_count = 1;
    struct InputRun
    {
        GCInputFrame input{};
        std::uint64_t epoch_count = 0;

        friend bool operator==(const InputRun&, const InputRun&) = default;
    };
    std::vector<InputRun> input_runs;
    std::string output_dtm_path;
    std::string output_savestate_path;
};

[[nodiscard]] std::vector<TasMovieInputEpochRewriteRequestV1::InputRun>
BuildRewriteInputRunsV1(
    const TasMovieInputEpochScheduleV1& schedule,
    std::uint64_t insert_before_epoch,
    std::uint64_t neutral_epoch_count);

struct TasMovieInputEpochRewriteResultV1
{
    InputEpochOutcomeV1 outcome = InputEpochOutcomeV1::Diverged;
    std::uint64_t insert_before_epoch = 0;
    std::uint64_t neutral_epoch_count = 0;
    std::uint64_t source_epoch_count = 0;
    std::uint64_t child_epoch_count = 0;
    std::uint64_t final_cursor = 0;
    InputEpochFailureReasonV1 failure_reason =
        InputEpochFailureReasonV1::None;
    std::uint64_t failure_epoch = 0;
    std::uint64_t expected_cursor = 0;
    std::uint64_t actual_cursor = 0;
    std::vector<program::ProgramArtifact> artifacts;
};

enum class TasMovieCutsceneEndpointV1 : std::int64_t
{
    PreBattleSeed = 1,
    FieldFastPreseed = 2,
    FieldDeferredPreseed = 3,
};

struct TasMovieCutsceneRequestV1
{
    std::uint64_t source_movie_input_cursor = 0;
    std::string output_dtm_path;
    std::string output_savestate_path;
};

struct TasMovieCutsceneResultV1
{
    TasMovieCutsceneEndpointV1 endpoint =
        TasMovieCutsceneEndpointV1::PreBattleSeed;
    std::uint32_t endpoint_pc = 0;
    std::uint64_t checkpoint_input_count = 0;
    std::uint64_t checkpoint_vi_count = 0;
    std::uint32_t area = 0;
    std::uint8_t subfield = 0;
    std::uint64_t final_input_count = 0;
    std::vector<program::ProgramArtifact> artifacts;
};

[[nodiscard]] GCInputFrame DecodeGuestPadStatusV1(
    std::uint64_t packed_status) noexcept;

[[nodiscard]] bool ValidateInputEpochScheduleV1(
    const TasMovieInputEpochScheduleV1& schedule,
    std::string* diagnostic = nullptr);
[[nodiscard]] std::vector<std::uint8_t> EncodeInputEpochScheduleArtifactV1(
    const TasMovieInputEpochScheduleV1& schedule,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodeInputEpochScheduleArtifactV1(
    std::span<const std::uint8_t> bytes,
    TasMovieInputEpochScheduleV1& schedule,
    std::string* diagnostic = nullptr);

[[nodiscard]] std::vector<std::uint8_t> EncodeAnnotationExecutionInputV1(
    const TasMovieInputEpochAnnotationRequestV1& request,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodeAnnotationExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    TasMovieInputEpochAnnotationRequestV1& request,
    std::string* diagnostic = nullptr);
[[nodiscard]] std::vector<std::uint8_t> EncodeRewriteExecutionInputV1(
    const TasMovieInputEpochRewriteRequestV1& request,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodeRewriteExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    TasMovieInputEpochRewriteRequestV1& request,
    std::string* diagnostic = nullptr);
[[nodiscard]] std::vector<std::uint8_t> EncodeCutsceneExecutionInputV1(
    const TasMovieCutsceneRequestV1& request,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodeCutsceneExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    TasMovieCutsceneRequestV1& request,
    std::string* diagnostic = nullptr);

class IAnnotationFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] fullphase::FullPhaseWorksetPolicy workset_policy()
        const noexcept final { return {1, 1}; }
    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded,
        TasMovieInputEpochAnnotationResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

class IRewriteFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] fullphase::FullPhaseWorksetPolicy workset_policy()
        const noexcept final { return {1, 1}; }
    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded,
        TasMovieInputEpochRewriteResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

class ICutsceneFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] fullphase::FullPhaseWorksetPolicy workset_policy()
        const noexcept final { return {1, 1}; }
    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded,
        TasMovieCutsceneResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

[[nodiscard]] std::shared_ptr<const IAnnotationFullPhaseDefinitionV1>
AnnotationFullPhaseDefinitionV1();
[[nodiscard]] std::shared_ptr<const IAnnotationFullPhaseDefinitionV1>
BreakpointDiagnosticFullPhaseDefinitionV1();
[[nodiscard]] std::shared_ptr<const IRewriteFullPhaseDefinitionV1>
RewriteFullPhaseDefinitionV1();
[[nodiscard]] std::shared_ptr<const ICutsceneFullPhaseDefinitionV1>
CutsceneFullPhaseDefinitionV1();

} // namespace savor::runtime::tasmovie::inputepoch
