#include "SpiceBattleContentAdapter.h"

#include "ActionViewStdJsonCache.h"
#include "ActionViewStdJsonLoader.h"
#include "SpiceBattleContentAdapterTestSeam.h"

#include "../third-party/SPICE/Sa3Dport/Animation/Motion.h"
#include "../third-party/SPICE/SpiceMLD/Parsing/MldParser.h"
#include "../third-party/SPICE/SpiceStd/StdParser.h"

#include <Utils/Hash.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <span>
#include <string_view>
#include <utility>

namespace savor::predict {
namespace {

constexpr std::string_view kPinnedSpiceRevision =
    "0b82fe1efa018c31ea9166cf3709c035b6bfe061";

struct FirstBattleResourceSpec {
    std::string_view stem;
    std::string_view primary_std_filename;
    std::string_view companion_std_filename;
    std::string_view mld_filename;
};

constexpr std::array<FirstBattleResourceSpec, 3> kFirstBattleResources{ {
    { "ma000", "ma000.std", "ma0000.std", "ma000.mld" },
    { "ma001", "ma001.std", "ma0010.std", "ma001.mld" },
    { "mb000", "mb000.std", "mb0000.std", "mb000.mld" },
} };

struct AdapterBuildContext {
    BattlePredictorResourceBundle bundle;
};

struct ResolvedFile {
    std::filesystem::path path;
    std::string relative_path;
    std::string normalized_relative_path;
};

struct LoadedBytes {
    ResolvedFile source;
    std::vector<std::uint8_t> bytes;
    BattlePredictorResourceSourceIdentity identity;
};

std::string lowercase_ascii(std::string_view value) {
    std::string out(value.begin(), value.end());
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool ascii_iequals(std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size()
        && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char left, unsigned char right) {
            return std::tolower(left) == std::tolower(right);
        });
}

std::string normalized_relative_path(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return lowercase_ascii(value);
}

void escalate_status(
    BattlePredictorResourceInputStatus& current,
    BattlePredictorResourceInputStatus candidate) {
    if (current == BattlePredictorResourceInputStatus::MissingInput
        || candidate == BattlePredictorResourceInputStatus::Ready) {
        return;
    }
    if (candidate == BattlePredictorResourceInputStatus::MissingInput
        || current == BattlePredictorResourceInputStatus::Ready) {
        current = candidate;
    }
}

void add_diagnostic(
    AdapterBuildContext& context,
    BattlePredictorResourceInputStatus classification,
    BattlePredictorResourceDiagnosticSeverity severity,
    std::string logical_role,
    std::string message,
    std::optional<std::uint32_t> source_offset = std::nullopt,
    BattlePredictorResourceTemplate* resource = nullptr) {
    BattlePredictorResourceDiagnostic diagnostic{
        .severity = severity,
        .logical_role = std::move(logical_role),
        .message = std::move(message),
        .source_offset = source_offset,
    };
    const auto same_diagnostic =
        [&](const BattlePredictorResourceDiagnostic& existing) {
            return existing.severity == diagnostic.severity
                && existing.logical_role == diagnostic.logical_role
                && existing.message == diagnostic.message
                && existing.source_offset == diagnostic.source_offset;
        };
    if (std::find_if(
            context.bundle.diagnostics.begin(),
            context.bundle.diagnostics.end(),
            same_diagnostic)
        == context.bundle.diagnostics.end()) {
        context.bundle.diagnostics.push_back(diagnostic);
    }
    if (resource != nullptr) {
        if (std::find_if(
                resource->diagnostics.begin(),
                resource->diagnostics.end(),
                same_diagnostic)
            == resource->diagnostics.end()) {
            resource->diagnostics.push_back(diagnostic);
        }
        escalate_status(resource->status, classification);
    }
    escalate_status(context.bundle.status, classification);
}

std::optional<std::filesystem::path> unique_case_insensitive_child(
    AdapterBuildContext& context,
    const std::filesystem::path& directory,
    std::string_view expected_name,
    bool require_directory,
    std::string_view logical_role) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            std::string(logical_role),
            "required input directory is unavailable: " + directory.string());
        return std::nullopt;
    }

    std::vector<std::filesystem::path> candidates;
    std::vector<std::string> candidate_names;
    std::filesystem::directory_iterator iterator(directory, ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            std::string(logical_role),
            "failed to enumerate input directory: " + directory.string()
                + ": " + ec.message());
        return std::nullopt;
    }
    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                std::string(logical_role),
                "failed while enumerating input directory: " + directory.string()
                    + ": " + ec.message());
            return std::nullopt;
        }
        std::error_code type_error;
        const bool type_matches = require_directory
            ? iterator->is_directory(type_error)
            : iterator->is_regular_file(type_error);
        if (!type_error && type_matches) {
            candidates.push_back(iterator->path());
            candidate_names.push_back(
                iterator->path().filename().string());
        }
    }

    const auto selected =
        adapter_test::select_case_insensitive_candidate(
            expected_name,
            candidate_names);
    if (selected.status
        == BattlePredictorResourceInputStatus::MissingInput) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            std::string(logical_role),
            "required input was not found case-insensitively: "
                + (directory / std::string(expected_name)).string());
        return std::nullopt;
    }
    if (selected.status
        == BattlePredictorResourceInputStatus::Ambiguous) {
        std::ostringstream detail;
        detail << "case-colliding inputs matched " << expected_name << ':';
        for (const auto& name : candidate_names) {
            if (ascii_iequals(name, expected_name)) {
                detail << ' ' << name;
            }
        }
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::Ambiguous,
            BattlePredictorResourceDiagnosticSeverity::Error,
            std::string(logical_role),
            detail.str());
        return std::nullopt;
    }
    if (!selected.selected_index.has_value()
        || *selected.selected_index >= candidates.size()) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            std::string(logical_role),
            "case-insensitive input selection returned no candidate");
        return std::nullopt;
    }
    return candidates[*selected.selected_index];
}

std::optional<std::filesystem::path> resolve_bchara_directory(
    AdapterBuildContext& context,
    const std::filesystem::path& disc_dump_root) {
    std::error_code ec;
    if (std::filesystem::is_directory(disc_dump_root, ec)
        && ascii_iequals(disc_dump_root.filename().string(), "bchara")) {
        return disc_dump_root;
    }
    return unique_case_insensitive_child(
        context,
        disc_dump_root,
        "bchara",
        true,
        "bchara_directory");
}

std::optional<ResolvedFile> resolve_source_file(
    AdapterBuildContext& context,
    const std::filesystem::path& directory,
    std::string_view expected_name,
    const std::filesystem::path& identity_root,
    std::string_view logical_role) {
    const auto path = unique_case_insensitive_child(
        context,
        directory,
        expected_name,
        false,
        logical_role);
    if (!path.has_value()) {
        return std::nullopt;
    }
    std::error_code ec;
    auto relative = std::filesystem::relative(*path, identity_root, ec);
    if (ec || relative.empty()) {
        relative = path->filename();
    }
    return ResolvedFile{
        .path = *path,
        .relative_path = relative.generic_string(),
        .normalized_relative_path =
            normalized_relative_path(relative.generic_string()),
    };
}

std::optional<LoadedBytes> read_source_bytes(
    AdapterBuildContext& context,
    const ResolvedFile& source,
    std::string logical_role,
    std::string parser_identity) {
    std::ifstream input(source.path, std::ios::binary | std::ios::ate);
    if (!input) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            logical_role,
            "failed to open required input: " + source.path.string());
        return std::nullopt;
    }
    const auto end = input.tellg();
    if (end < 0) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            logical_role,
            "failed to determine required input size: " + source.path.string());
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()
        && !input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            logical_role,
            "failed to read required input: " + source.path.string());
        return std::nullopt;
    }

    const auto digest = hash::sha256(bytes.data(), bytes.size());
    if (digest.empty()) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            logical_role,
            "failed to hash required input: " + source.path.string());
        return std::nullopt;
    }
    return LoadedBytes{
        .source = source,
        .bytes = std::move(bytes),
        .identity = BattlePredictorResourceSourceIdentity{
            .logical_role = std::move(logical_role),
            .relative_path = source.relative_path,
            .normalized_relative_path = source.normalized_relative_path,
            .source_path = source.path.string(),
            .size_bytes = static_cast<std::uint64_t>(end),
            .sha256 = digest,
            .parser_identity = std::move(parser_identity)
                + "@spice-" + context.bundle.spice_revision,
        },
    };
}

void append_source_diagnostic(
    AdapterBuildContext& context,
    LoadedBytes& source,
    BattlePredictorResourceDiagnosticSeverity severity,
    std::string message,
    std::optional<std::uint32_t> source_offset = std::nullopt) {
    BattlePredictorResourceDiagnostic diagnostic{
        .severity = severity,
        .logical_role = source.identity.logical_role,
        .message = std::move(message),
        .source_offset = source_offset,
    };
    const auto duplicate = std::find_if(
        source.identity.diagnostics.begin(),
        source.identity.diagnostics.end(),
        [&](const BattlePredictorResourceDiagnostic& existing) {
            return existing.severity == diagnostic.severity
                && existing.logical_role == diagnostic.logical_role
                && existing.message == diagnostic.message
                && existing.source_offset == diagnostic.source_offset;
        });
    if (duplicate != source.identity.diagnostics.end()) {
        return;
    }
    source.identity.diagnostics.push_back(diagnostic);
    context.bundle.diagnostics.push_back(std::move(diagnostic));
}

void finish_source(
    AdapterBuildContext& context,
    LoadedBytes&& source,
    std::string parser_status) {
    source.identity.parser_status = std::move(parser_status);
    context.bundle.sources.push_back(std::move(source.identity));
}

std::optional<spice::stdfile::StdFile> load_direct_std(
    AdapterBuildContext& context,
    const ResolvedFile& source,
    std::string logical_role,
    spice::stdfile::StdLayoutKind required_layout) {
    auto loaded = read_source_bytes(
        context,
        source,
        std::move(logical_role),
        "spice::stdfile::parseBytes");
    if (!loaded.has_value()) {
        return std::nullopt;
    }

    auto file = spice::stdfile::parseBytes(
        std::move(loaded->bytes),
        loaded->source.path.string());
    for (const auto& diagnostic : file.diagnostics) {
        BattlePredictorResourceDiagnosticSeverity severity =
            BattlePredictorResourceDiagnosticSeverity::Info;
        if (diagnostic.severity == spice::stdfile::StdDiagnosticSeverity::Warning) {
            severity = BattlePredictorResourceDiagnosticSeverity::Warning;
        } else if (diagnostic.severity == spice::stdfile::StdDiagnosticSeverity::Error) {
            severity = BattlePredictorResourceDiagnosticSeverity::Error;
        }
        append_source_diagnostic(
            context,
            *loaded,
            severity,
            diagnostic.message,
            diagnostic.offset);
    }

    const bool parse_ok = file.ok();
    const bool layout_ok = file.layoutKind == required_layout;
    finish_source(
        context,
        std::move(*loaded),
        parse_ok
            ? spice::stdfile::toString(file.layoutKind)
            : "error");
    if (!parse_ok) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            file.sourcePath,
            "SPICE failed to parse required STD input");
        return std::nullopt;
    }
    if (!layout_ok) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            file.sourcePath,
            "STD layout mismatch: expected "
                + std::string(spice::stdfile::toString(required_layout))
                + ", observed "
                + spice::stdfile::toString(file.layoutKind));
        return std::nullopt;
    }
    return file;
}

std::vector<CombatantStdActionRow> project_action_rows(
    AdapterBuildContext& context,
    const spice::stdfile::StdFile& file,
    std::string_view logical_role,
    BattlePredictorResourceTemplate* resource) {
    std::vector<CombatantStdActionRow> result;
    result.reserve(file.actionRows.rows.size());
    for (const auto& row : file.actionRows.rows) {
        if (row.index > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                std::string(logical_role),
                "STD action-row index cannot be represented by the predictor",
                row.decodedOffset,
                resource);
            continue;
        }
        result.push_back(CombatantStdActionRow{
            .index = static_cast<int>(row.index),
            .action_id = row.actionId,
            .row_type = row.rowType,
            .callback_index = row.callbackIndex,
            .callback_ordinal = row.motionSlotOrdinal,
            .flags = row.flags,
            .secondary_key = row.secondaryKey,
            .callback_aux_param = row.callbackAuxParam,
            .transition_gate_divisor_bits =
                row.selectionTransitionScalarBits,
            .motion_progress_step_bits = row.motionProgressScalarBits,
        });
    }
    if (result.empty()) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            std::string(logical_role),
            "required primary STD did not contain any action rows",
            std::nullopt,
            resource);
    }
    return result;
}

std::vector<SpiceStdEntryProjection> project_entry_records(
    AdapterBuildContext& context,
    const spice::stdfile::StdFile& file,
    std::string_view logical_role,
    BattlePredictorResourceTemplate* resource) {
    std::vector<SpiceStdEntryProjection> result;
    result.reserve(file.entryTable.records.size());
    for (const auto& record : file.entryTable.records) {
        if (record.index
            > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            || record.payloadSize
            > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                std::string(logical_role),
                "STD entry record cannot be represented by the predictor",
                record.tableOffset,
                resource);
            continue;
        }
        const auto expected_combined = std0_combined_entry_id(
            record.locationCode,
            record.opcode);
        if (expected_combined != record.combinedType) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                std::string(logical_role),
                "STD entry combined type conflicts with location/opcode",
                record.tableOffset,
                resource);
            continue;
        }

        SpiceStdEntryProjection projected{
            .index = static_cast<int>(record.index),
            .location_code = record.locationCode,
            .opcode = record.opcode,
            .payload_size = static_cast<int>(record.payloadSize),
            .payload_in_bounds = record.payloadInBounds,
        };
        if (!record.isSentinel) {
            const auto offset = static_cast<std::size_t>(record.payloadOffsetAbs);
            const auto size = static_cast<std::size_t>(record.payloadSize);
            const bool independently_in_bounds =
                offset <= file.decodedBytes.size()
                && size <= file.decodedBytes.size() - offset;
            if (!record.payloadInBounds || !independently_in_bounds) {
                add_diagnostic(
                    context,
                    BattlePredictorResourceInputStatus::MissingInput,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    std::string(logical_role),
                    "required STD payload span is out of bounds",
                    record.tableOffset,
                    resource);
                projected.payload_in_bounds = false;
            } else {
                projected.payload_bytes.assign(
                    file.decodedBytes.begin()
                        + static_cast<std::ptrdiff_t>(offset),
                    file.decodedBytes.begin()
                        + static_cast<std::ptrdiff_t>(offset + size));
            }
        }
        result.push_back(std::move(projected));
        if (record.isSentinel) {
            break;
        }
    }
    if (result.empty()) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            std::string(logical_role),
            "required companion STD did not contain any entry records",
            std::nullopt,
            resource);
    }
    return result;
}

Std0Table make_runtime_aux_table(
    const std::vector<CombatantStdActionRow>& action_rows,
    const Std0Table& companion,
    bool* has_prefix,
    int* prefix_rows) {
    Std0Table runtime;
    if (!action_rows.empty()) {
        const auto& row = action_rows.front();
        runtime.entries.push_back(Std0EntryRecord{
            .location_code = static_cast<std::int16_t>(row.index),
            .opcode = row.row_type,
            .payload = Std0PayloadGateFields{
                .primary_action_key = row.action_id,
                .generic_secondary_key = row.row_type,
                .direct_gate_secondary_key = row.callback_index,
            },
            .has_payload = true,
        });
    }
    runtime.entries.insert(
        runtime.entries.end(),
        companion.entries.begin(),
        companion.entries.end());
    runtime.includes_sentinel = companion.includes_sentinel;
    if (has_prefix != nullptr) {
        *has_prefix = !action_rows.empty();
    }
    if (prefix_rows != nullptr) {
        *prefix_rows = action_rows.empty() ? 0 : 1;
    }
    return runtime;
}

std::optional<spice::mld::model::MldFile> load_direct_mld(
    AdapterBuildContext& context,
    const ResolvedFile& source,
    std::string logical_role) {
    auto loaded = read_source_bytes(
        context,
        source,
        std::move(logical_role),
        "spice::mld::parsing::MldParser::parseBytes");
    if (!loaded.has_value()) {
        return std::nullopt;
    }

    spice::mld::model::MldFile file;
    try {
        const spice::mld::parsing::MldParser parser;
        file = parser.parseBytes(loaded->bytes);
    } catch (const std::exception& exception) {
        finish_source(context, std::move(*loaded), "exception");
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            source.path.string(),
            "SPICE MLD parser threw: " + std::string(exception.what()));
        return std::nullopt;
    }

    bool has_error = false;
    for (const auto& diagnostic : file.parseDiagnostics) {
        BattlePredictorResourceDiagnosticSeverity severity =
            BattlePredictorResourceDiagnosticSeverity::Info;
        if (diagnostic.severity
            == spice::mld::model::MldDiagnostic::Severity::Warning) {
            severity = BattlePredictorResourceDiagnosticSeverity::Warning;
        } else if (diagnostic.severity
            == spice::mld::model::MldDiagnostic::Severity::Error) {
            severity = BattlePredictorResourceDiagnosticSeverity::Error;
            has_error = true;
        }
        append_source_diagnostic(
            context,
            *loaded,
            severity,
            diagnostic.message,
            diagnostic.sourceOffset);
    }
    std::string parse_status = "partial";
    switch (file.parseStatus) {
    case spice::mld::model::MldParseStatus::Complete:
        parse_status = "complete";
        break;
    case spice::mld::model::MldParseStatus::Partial:
        parse_status = "partial";
        break;
    case spice::mld::model::MldParseStatus::Failed:
        parse_status = "failed";
        has_error = true;
        break;
    case spice::mld::model::MldParseStatus::Empty:
        parse_status = "empty";
        has_error = true;
        break;
    }
    finish_source(context, std::move(*loaded), parse_status);
    if (has_error) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            source.path.string(),
            "SPICE did not completely parse the required MLD structure");
        return std::nullopt;
    }
    return file;
}

void project_mld_catalog(
    AdapterBuildContext& context,
    const spice::mld::model::MldFile& file,
    std::string_view logical_role,
    BattlePredictorResourceTemplate& resource) {
    adapter_test::MldCatalogProjectionInput input{
        .logical_role = std::string(logical_role),
    };
    input.entries.reserve(file.entries.size());
    for (const auto& record : file.entries) {
        adapter_test::MldCatalogEntryInput entry{
            .table_index = record.entry.tableIndex,
            .source_entry_id = record.entry.entryId,
            .has_motion_address_list =
                record.entry.motionAddresses != nullptr,
        };
        if (record.entry.motionAddresses) {
            entry.motion_addresses = record.entry.motionAddresses->values;
        }
        entry.declared_nonzero_motion_slots = record.entry.motionCount;
        input.entries.push_back(std::move(entry));
    }
    input.resources.reserve(file.motionResources.size());
    for (const auto& [address, source] : file.motionResources) {
        adapter_test::MldCatalogResourceInput projected{
            .source_motion_address = address,
        };
        projected.variants.reserve(source.variants.size());
        for (const auto& variant : source.variants) {
            projected.variants.push_back(adapter_test::MldCatalogVariantInput{
                .node_count = variant.nodeCount,
                .short_rotation = variant.shortRot,
                .has_motion = variant.motion != nullptr,
                .declared_frame_count =
                    variant.motion ? variant.motion->declared_frame_count : 0U,
            });
        }
        input.resources.push_back(std::move(projected));
    }
    input.bindings.reserve(file.animationBindings.size());
    for (const auto& binding : file.animationBindings) {
        input.bindings.push_back(adapter_test::MldCatalogBindingInput{
            .table_index = binding.tableIndex,
            .source_entry_id = binding.sourceEntryId,
            .motion_slot = binding.motionSlot,
            .source_motion_address = binding.motionAddress,
            .source_object_address = binding.objectAddress,
            .node_count = binding.nodeCount,
            .short_rotation = binding.shortRot,
            .variant_index = binding.motionVariantIndex,
        });
    }

    auto projected = adapter_test::project_mld_motion_catalog(input);
    resource.mld_entries = std::move(projected.entries);
    resource.visual_resource.motion_frame_counts =
        std::move(projected.motion_frame_counts);
    for (auto& diagnostic : projected.diagnostics) {
        add_diagnostic(
            context,
            diagnostic.classification,
            diagnostic.diagnostic.severity,
            diagnostic.diagnostic.logical_role,
            diagnostic.diagnostic.message,
            diagnostic.diagnostic.source_offset,
            &resource);
    }
}

bool load_direct_std_resource(
    AdapterBuildContext& context,
    const std::filesystem::path& bchara_dir,
    const std::filesystem::path& disc_dump_root,
    const FirstBattleResourceSpec& spec,
    BattlePredictorResourceTemplate& resource) {
    const auto stem = std::string(spec.stem);
    const auto primary_role = stem + ".primary_std";
    const auto companion_role = stem + ".companion_std";
    const auto primary_source = resolve_source_file(
        context,
        bchara_dir,
        spec.primary_std_filename,
        disc_dump_root,
        primary_role);
    const auto companion_source = resolve_source_file(
        context,
        bchara_dir,
        spec.companion_std_filename,
        disc_dump_root,
        companion_role);
    if (!primary_source.has_value() || !companion_source.has_value()) {
        escalate_status(
            resource.status,
            context.bundle.status == BattlePredictorResourceInputStatus::Ambiguous
                ? BattlePredictorResourceInputStatus::Ambiguous
                : BattlePredictorResourceInputStatus::MissingInput);
        return false;
    }

    const auto primary = load_direct_std(
        context,
        *primary_source,
        primary_role,
        spice::stdfile::StdLayoutKind::ActionRows);
    const auto companion = load_direct_std(
        context,
        *companion_source,
        companion_role,
        spice::stdfile::StdLayoutKind::EntryTable);
    if (!primary.has_value() || !companion.has_value()) {
        escalate_status(
            resource.status,
            context.bundle.status == BattlePredictorResourceInputStatus::Ambiguous
                ? BattlePredictorResourceInputStatus::Ambiguous
                : BattlePredictorResourceInputStatus::MissingInput);
        return false;
    }

    resource.visual_resource.action_rows =
        project_action_rows(context, *primary, primary_role, &resource);
    const auto projected =
        project_entry_records(context, *companion, companion_role, &resource);
    const auto visual = project_spice_std_visual_resource(projected);
    const auto std0 = project_spice_std0_table(projected);
    if (!visual.ok || !std0.ok) {
        for (const auto& error : visual.errors) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                companion_role,
                error,
                std::nullopt,
                &resource);
        }
        for (const auto& error : std0.errors) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                companion_role,
                error,
                std::nullopt,
                &resource);
        }
        return false;
    }
    const auto action_rows = std::move(resource.visual_resource.action_rows);
    resource.visual_resource = visual.resource;
    resource.visual_resource.action_rows = action_rows;
    resource.visual_resource.provenance =
        "direct SPICE StdFile projection from " + companion_source->path.string();
    resource.companion_std0_table = std0.table;
    resource.runtime_aux_table = make_runtime_aux_table(
        resource.visual_resource.action_rows,
        resource.companion_std0_table,
        &resource.runtime_aux_table_has_action_row_prefix,
        &resource.runtime_aux_table_prefix_rows);
    return resource.status == BattlePredictorResourceInputStatus::Ready;
}

std::optional<LoadedBytes> load_legacy_json_source(
    AdapterBuildContext& context,
    const std::filesystem::path& json_dir,
    std::string_view filename,
    std::string logical_role) {
    const auto source = resolve_source_file(
        context,
        json_dir,
        filename,
        json_dir,
        logical_role);
    if (!source.has_value()) {
        return std::nullopt;
    }
    return read_source_bytes(
        context,
        *source,
        std::move(logical_role),
        "savor::predict::spice_std_ir_v1_json_compatibility_loader");
}

bool verify_legacy_std_manifest(
    AdapterBuildContext& context,
    const std::filesystem::path& json_dir) {
    constexpr std::string_view role = "legacy_std_json_manifest";
    const auto manifest_source = resolve_source_file(
        context,
        json_dir,
        action_view_std_json_manifest_filename(),
        json_dir,
        role);
    std::optional<LoadedBytes> manifest_bytes;
    if (manifest_source.has_value()) {
        manifest_bytes = read_source_bytes(
            context,
            *manifest_source,
            std::string(role),
            "savor_action_view_std_manifest_v1_verifier");
    }

    const auto verification = resolve_action_view_std_json_cache({
        .explicit_std_json_dir = json_dir,
    });
    if (manifest_bytes.has_value()) {
        finish_source(
            context,
            std::move(*manifest_bytes),
            verification.manifest_verified ? "verified" : "rejected");
    }
    if (!verification.manifest_verified || !verification.available) {
        for (const auto& diagnostic : verification.diagnostics) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                std::string(role),
                diagnostic);
        }
        for (const auto& mismatch : verification.hash_mismatches) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                std::string(role),
                mismatch);
        }
        if (verification.diagnostics.empty()
            && verification.hash_mismatches.empty()) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                std::string(role),
                "explicit legacy STD JSON directory failed manifest verification");
        }
        return false;
    }
    return true;
}

std::string bytes_as_string(const std::vector<std::uint8_t>& bytes) {
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
}

bool load_legacy_std_resource(
    AdapterBuildContext& context,
    const std::filesystem::path& json_dir,
    const FirstBattleResourceSpec& spec,
    BattlePredictorResourceTemplate& resource) {
    const auto stem = std::string(spec.stem);
    const auto primary_role = stem + ".primary_std_json";
    const auto companion_role = stem + ".companion_std_json";
    auto primary = load_legacy_json_source(
        context,
        json_dir,
        std::string(spec.primary_std_filename) + ".json",
        primary_role);
    auto companion = load_legacy_json_source(
        context,
        json_dir,
        std::string(spec.companion_std_filename) + ".json",
        companion_role);
    if (!primary.has_value() || !companion.has_value()) {
        escalate_status(
            resource.status,
            context.bundle.status == BattlePredictorResourceInputStatus::Ambiguous
                ? BattlePredictorResourceInputStatus::Ambiguous
                : BattlePredictorResourceInputStatus::MissingInput);
        return false;
    }

    const auto primary_text = bytes_as_string(primary->bytes);
    const auto companion_text = bytes_as_string(companion->bytes);
    auto action_rows = load_spice_std_action_rows_from_json_text(primary_text);
    auto visual =
        load_spice_std_visual_resource_from_json_text(companion_text);
    auto std0 = load_spice_std0_table_from_json_text(companion_text);
    finish_source(
        context,
        std::move(*primary),
        action_rows.ok ? "action_rows" : "error");
    finish_source(
        context,
        std::move(*companion),
        visual.ok && std0.ok ? "entry_table" : "error");

    for (const auto& error : action_rows.errors) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            primary_role,
            error,
            std::nullopt,
            &resource);
    }
    for (const auto& error : visual.errors) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            companion_role,
            error,
            std::nullopt,
            &resource);
    }
    for (const auto& error : std0.errors) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            companion_role,
            error,
            std::nullopt,
            &resource);
    }
    if (!action_rows.ok || !visual.ok || !std0.ok) {
        return false;
    }

    resource.visual_resource = std::move(visual.resource);
    resource.visual_resource.action_rows = std::move(action_rows.rows);
    resource.visual_resource.provenance =
        "explicit legacy spice_std_ir_v1 JSON compatibility provider";
    resource.companion_std0_table = std::move(std0.table);
    resource.runtime_aux_table = make_runtime_aux_table(
        resource.visual_resource.action_rows,
        resource.companion_std0_table,
        &resource.runtime_aux_table_has_action_row_prefix,
        &resource.runtime_aux_table_prefix_rows);
    return resource.status == BattlePredictorResourceInputStatus::Ready;
}

void load_resource_mld(
    AdapterBuildContext& context,
    const std::filesystem::path& bchara_dir,
    const std::filesystem::path& disc_dump_root,
    const FirstBattleResourceSpec& spec,
    BattlePredictorResourceTemplate& resource) {
    const auto role = std::string(spec.stem) + ".mld";
    const auto source = resolve_source_file(
        context,
        bchara_dir,
        spec.mld_filename,
        disc_dump_root,
        role);
    if (!source.has_value()) {
        escalate_status(
            resource.status,
            context.bundle.status == BattlePredictorResourceInputStatus::Ambiguous
                ? BattlePredictorResourceInputStatus::Ambiguous
                : BattlePredictorResourceInputStatus::MissingInput);
        return;
    }
    const auto mld = load_direct_mld(context, *source, role);
    if (!mld.has_value()) {
        escalate_status(
            resource.status,
            BattlePredictorResourceInputStatus::MissingInput);
        return;
    }
    project_mld_catalog(context, *mld, role, resource);
}

void load_direct_damage_resource(
    AdapterBuildContext& context,
    const std::filesystem::path& bchara_dir,
    const std::filesystem::path& disc_dump_root) {
    constexpr std::string_view role = "damage.target_reaction_std";
    const auto source = resolve_source_file(
        context,
        bchara_dir,
        "damage.std",
        disc_dump_root,
        role);
    if (!source.has_value()) {
        return;
    }
    const auto parsed = load_direct_std(
        context,
        *source,
        std::string(role),
        spice::stdfile::StdLayoutKind::EntryTable);
    if (!parsed.has_value()) {
        return;
    }
    const auto projected =
        project_entry_records(context, *parsed, role, nullptr);
    auto visual = project_spice_std_visual_resource(projected);
    if (!visual.ok) {
        for (const auto& error : visual.errors) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                std::string(role),
                error);
        }
        return;
    }
    visual.resource.binding = {
        .slot = -1,
        .resource_stem = "damage",
    };
    visual.resource.provenance =
        "direct SPICE StdFile projection from " + source->path.string();
    context.bundle.target_reaction_effect_resource =
        std::move(visual.resource);
}

void load_legacy_damage_resource(
    AdapterBuildContext& context,
    const std::filesystem::path& json_dir) {
    constexpr std::string_view role = "damage.target_reaction_std_json";
    auto source = load_legacy_json_source(
        context,
        json_dir,
        "damage.std.json",
        std::string(role));
    if (!source.has_value()) {
        return;
    }
    const auto text = bytes_as_string(source->bytes);
    auto visual = load_spice_std_visual_resource_from_json_text(text);
    finish_source(
        context,
        std::move(*source),
        visual.ok ? "entry_table" : "error");
    if (!visual.ok) {
        for (const auto& error : visual.errors) {
            add_diagnostic(
                context,
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                std::string(role),
                error);
        }
        return;
    }
    visual.resource.binding = {
        .slot = -1,
        .resource_stem = "damage",
    };
    visual.resource.provenance =
        "explicit legacy spice_std_ir_v1 JSON compatibility provider";
    context.bundle.target_reaction_effect_resource =
        std::move(visual.resource);
}

std::string compute_bundle_digest(
    const BattlePredictorResourceBundle& bundle) {
    std::vector<const BattlePredictorResourceSourceIdentity*> sources;
    sources.reserve(bundle.sources.size());
    for (const auto& source : bundle.sources) {
        sources.push_back(&source);
    }
    std::sort(
        sources.begin(),
        sources.end(),
        [](const auto* lhs, const auto* rhs) {
            if (lhs->logical_role != rhs->logical_role) {
                return lhs->logical_role < rhs->logical_role;
            }
            return lhs->normalized_relative_path
                < rhs->normalized_relative_path;
        });

    std::ostringstream canonical;
    canonical
        << "battle_predictor_resource_bundle_v1\n"
        << battle_predictor_resource_provider_kind_name(bundle.provider_kind)
        << '\n'
        << bundle.adapter_version << '\n'
        << bundle.spice_revision << '\n';
    for (const auto* source : sources) {
        canonical
            << source->logical_role << '\n'
            << source->normalized_relative_path << '\n'
            << source->size_bytes << '\n'
            << source->sha256 << '\n'
            << source->parser_identity << '\n';
    }
    const auto text = canonical.str();
    return hash::sha256(text.data(), text.size());
}

} // namespace

adapter_test::CaseInsensitiveCandidateSelection
adapter_test::select_case_insensitive_candidate(
    std::string_view expected_name,
    std::span<const std::string> candidate_names) {
    const auto equal_ascii = [](
                                 std::string_view lhs,
                                 std::string_view rhs) {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t index = 0; index < lhs.size(); ++index) {
            const auto lower = [](unsigned char value) {
                return value >= static_cast<unsigned char>('A')
                        && value <= static_cast<unsigned char>('Z')
                    ? static_cast<unsigned char>(
                        value - static_cast<unsigned char>('A')
                        + static_cast<unsigned char>('a'))
                    : value;
            };
            if (lower(static_cast<unsigned char>(lhs[index]))
                != lower(static_cast<unsigned char>(rhs[index]))) {
                return false;
            }
        }
        return true;
    };

    CaseInsensitiveCandidateSelection result;
    for (std::size_t index = 0; index < candidate_names.size(); ++index) {
        if (!equal_ascii(candidate_names[index], expected_name)) {
            continue;
        }
        if (result.selected_index.has_value()) {
            result.status =
                BattlePredictorResourceInputStatus::Ambiguous;
            result.selected_index.reset();
            return result;
        }
        result.selected_index = index;
    }
    if (result.selected_index.has_value()) {
        result.status = BattlePredictorResourceInputStatus::Ready;
    }
    return result;
}

adapter_test::MldCatalogProjectionResult
adapter_test::project_mld_motion_catalog(
    const MldCatalogProjectionInput& input) {
    MldCatalogProjectionResult result;
    const auto logical_role =
        input.logical_role.empty() ? std::string("mld") : input.logical_role;
    const auto report = [&](
                            BattlePredictorResourceInputStatus classification,
                            BattlePredictorResourceDiagnosticSeverity severity,
                            std::string message,
                            std::optional<std::uint32_t> source_offset =
                                std::nullopt) {
        escalate_status(result.status, classification);
        result.diagnostics.push_back(MldCatalogProjectionDiagnostic{
            .classification = classification,
            .diagnostic = BattlePredictorResourceDiagnostic{
                .severity = severity,
                .logical_role = logical_role,
                .message = std::move(message),
                .source_offset = source_offset,
            },
        });
    };

    for (const auto& diagnostic : input.diagnostics) {
        report(
            diagnostic.severity
                    == BattlePredictorResourceDiagnosticSeverity::Error
                ? BattlePredictorResourceInputStatus::MissingInput
                : BattlePredictorResourceInputStatus::Ready,
            diagnostic.severity,
            diagnostic.message,
            diagnostic.source_offset);
    }
    if (input.entries.empty()) {
        report(
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            "required MLD did not contain any index entries");
        return result;
    }

    std::map<std::int16_t, std::uint32_t> flattened_frame_counts;
    result.entries.reserve(input.entries.size());
    for (const auto& entry : input.entries) {
        BattlePredictorMldEntryCatalog catalog{
            .table_index = entry.table_index,
            .source_entry_id = entry.source_entry_id,
        };
        if (!entry.has_motion_address_list) {
            report(
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                "MLD entry has no canonical motion-address slot list");
            result.entries.push_back(std::move(catalog));
            continue;
        }

        catalog.motion_slots.reserve(entry.motion_addresses.size());
        catalog.declared_nonzero_motion_slots =
            static_cast<std::size_t>(std::count_if(
                entry.motion_addresses.begin(),
                entry.motion_addresses.end(),
                [](std::uint32_t address) { return address != 0U; }));
        if (entry.declared_nonzero_motion_slots
            != catalog.declared_nonzero_motion_slots) {
            report(
                BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                "MLD canonical motionCount conflicts with the full slot list");
        }

        for (std::size_t slot_index = 0;
             slot_index < entry.motion_addresses.size();
             ++slot_index) {
            const auto address = entry.motion_addresses[slot_index];
            BattlePredictorMldMotionSlot slot{
                .table_index = entry.table_index,
                .source_entry_id = entry.source_entry_id,
                .motion_slot = slot_index,
                .source_motion_address = address,
                .has_motion_address = address != 0U,
                .decoded = false,
                .provenance = address == 0U
                    ? "canonical MldFile motion-address list; known empty slot"
                    : "canonical MldFile animation binding; declared_frame_count",
            };
            if (address == 0U) {
                catalog.motion_slots.push_back(std::move(slot));
                continue;
            }

            std::vector<const MldCatalogBindingInput*> bindings;
            for (const auto& binding : input.bindings) {
                if (binding.table_index == entry.table_index
                    && binding.source_entry_id == entry.source_entry_id
                    && binding.motion_slot == slot_index
                    && binding.source_motion_address == address) {
                    bindings.push_back(&binding);
                }
            }
            if (bindings.empty()) {
                report(
                    BattlePredictorResourceInputStatus::MissingInput,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    "nonzero MLD motion slot has no compatible animation binding: entry="
                        + std::to_string(entry.table_index)
                        + ", slot=" + std::to_string(slot_index),
                    address);
                catalog.motion_slots.push_back(std::move(slot));
                continue;
            }
            if (bindings.size() != 1U) {
                report(
                    BattlePredictorResourceInputStatus::Ambiguous,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    "nonzero MLD motion slot has multiple compatible animation bindings: entry="
                        + std::to_string(entry.table_index)
                        + ", slot=" + std::to_string(slot_index),
                    address);
                catalog.motion_slots.push_back(std::move(slot));
                continue;
            }
            const auto& binding = *bindings.front();

            std::vector<const MldCatalogResourceInput*> resources;
            for (const auto& candidate : input.resources) {
                if (candidate.source_motion_address == address) {
                    resources.push_back(&candidate);
                }
            }
            if (resources.empty()) {
                report(
                    BattlePredictorResourceInputStatus::MissingInput,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    "nonzero MLD motion slot points to a missing motion resource",
                    address);
                catalog.motion_slots.push_back(std::move(slot));
                continue;
            }
            if (resources.size() != 1U) {
                report(
                    BattlePredictorResourceInputStatus::Ambiguous,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    "nonzero MLD motion slot has duplicate motion resources",
                    address);
                catalog.motion_slots.push_back(std::move(slot));
                continue;
            }
            const auto& variants = resources.front()->variants;
            const auto compatible_count =
                static_cast<std::size_t>(std::count_if(
                    variants.begin(),
                    variants.end(),
                    [&](const auto& candidate) {
                        return candidate.node_count == binding.node_count
                            && candidate.short_rotation
                                == binding.short_rotation;
                    }));
            if (compatible_count == 0U
                || binding.variant_index >= variants.size()) {
                report(
                    BattlePredictorResourceInputStatus::MissingInput,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    "MLD animation binding does not resolve to a compatible retained variant",
                    address);
                catalog.motion_slots.push_back(std::move(slot));
                continue;
            }
            if (compatible_count != 1U) {
                report(
                    BattlePredictorResourceInputStatus::Ambiguous,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    "MLD animation binding has multiple compatible retained variants",
                    address);
                catalog.motion_slots.push_back(std::move(slot));
                continue;
            }
            const auto& variant = variants[binding.variant_index];
            if (variant.node_count != binding.node_count
                || variant.short_rotation != binding.short_rotation
                || !variant.has_motion) {
                report(
                    BattlePredictorResourceInputStatus::MissingInput,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    "MLD animation binding selected an incompatible or null motion",
                    address);
                catalog.motion_slots.push_back(std::move(slot));
                continue;
            }
            if (slot_index
                > static_cast<std::size_t>(
                    std::numeric_limits<std::int16_t>::max())) {
                report(
                    BattlePredictorResourceInputStatus::MissingInput,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    "MLD motion slot cannot be represented by the predictor",
                    address);
                catalog.motion_slots.push_back(std::move(slot));
                continue;
            }

            const auto motion_id =
                static_cast<std::int16_t>(slot_index);
            const auto frame_count = variant.declared_frame_count;
            const auto existing = flattened_frame_counts.find(motion_id);
            if (existing != flattened_frame_counts.end()) {
                report(
                    BattlePredictorResourceInputStatus::Ambiguous,
                    BattlePredictorResourceDiagnosticSeverity::Error,
                    existing->second == frame_count
                        ? "MLD has multiple eligible entry owners for one motion slot"
                        : "MLD has conflicting declared frame counts for one motion slot",
                    address);
            } else {
                flattened_frame_counts.emplace(motion_id, frame_count);
            }

            slot.decoded = true;
            slot.declared_frame_count = frame_count;
            slot.source_object_address =
                binding.source_object_address;
            slot.node_count = binding.node_count;
            slot.short_rotation = binding.short_rotation;
            ++catalog.decoded_nonzero_motion_slots;
            catalog.motion_slots.push_back(std::move(slot));
        }

        catalog.complete =
            catalog.decoded_nonzero_motion_slots
            == catalog.declared_nonzero_motion_slots;
        if (!catalog.complete) {
            report(
                result.status
                        == BattlePredictorResourceInputStatus::Ambiguous
                    ? BattlePredictorResourceInputStatus::Ambiguous
                    : BattlePredictorResourceInputStatus::MissingInput,
                BattlePredictorResourceDiagnosticSeverity::Error,
                "MLD nonzero motion-slot catalog is incomplete for entry "
                    + std::to_string(entry.table_index));
        }
        result.entries.push_back(std::move(catalog));
    }

    result.motion_frame_counts.reserve(flattened_frame_counts.size());
    for (const auto& [motion_id, frame_count] : flattened_frame_counts) {
        result.motion_frame_counts.push_back(
            CombatantVisualMotionFrameCount{
                .motion_id = motion_id,
                .frame_count = frame_count,
                .provenance =
                    "direct SPICE canonical MldFile Motion::declared_frame_count",
            });
    }
    return result;
}

SpiceBattleContentAdapterResult load_first_battle_spice_resource_bundle(
    const SpiceBattleContentAdapterOptions& options) {
    AdapterBuildContext context;
    context.bundle.status = BattlePredictorResourceInputStatus::Ready;
    context.bundle.provider_kind = options.legacy_std_json_dir.empty()
        ? BattlePredictorResourceProviderKind::DirectSpice
        : BattlePredictorResourceProviderKind::LegacyStdJsonDirectMld;
    context.bundle.spice_revision = options.spice_revision.empty()
        ? std::string(kPinnedSpiceRevision)
        : options.spice_revision;

    if (options.disc_dump_root.empty()) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            "disc_dump_root",
            "direct SPICE resource loading requires a disc-dump root");
    }
    const auto bchara_dir = options.disc_dump_root.empty()
        ? std::optional<std::filesystem::path>{}
        : resolve_bchara_directory(context, options.disc_dump_root);
    auto disc_source_identity_root = options.disc_dump_root;
    if (bchara_dir.has_value()
        && ascii_iequals(
            options.disc_dump_root.lexically_normal().filename().string(),
            "bchara")) {
        disc_source_identity_root = bchara_dir->parent_path();
        if (disc_source_identity_root.empty()) {
            disc_source_identity_root = ".";
        }
    }
    if (!options.legacy_std_json_dir.empty()) {
        verify_legacy_std_manifest(
            context,
            options.legacy_std_json_dir);
    }

    for (const auto& spec : kFirstBattleResources) {
        BattlePredictorResourceTemplate resource;
        resource.resource_stem = std::string(spec.stem);
        resource.status = BattlePredictorResourceInputStatus::Ready;
        resource.visual_resource.binding = {
            .slot = -1,
            .resource_stem = resource.resource_stem,
        };

        if (options.legacy_std_json_dir.empty()) {
            if (bchara_dir.has_value()) {
                load_direct_std_resource(
                    context,
                    *bchara_dir,
                    disc_source_identity_root,
                    spec,
                    resource);
            } else {
                escalate_status(
                    resource.status,
                    BattlePredictorResourceInputStatus::MissingInput);
            }
        } else {
            load_legacy_std_resource(
                context,
                options.legacy_std_json_dir,
                spec,
                resource);
        }
        resource.visual_resource.binding = {
            .slot = -1,
            .resource_stem = resource.resource_stem,
        };

        if (bchara_dir.has_value()) {
            load_resource_mld(
                context,
                *bchara_dir,
                disc_source_identity_root,
                spec,
                resource);
        } else {
            escalate_status(
                resource.status,
                BattlePredictorResourceInputStatus::MissingInput);
        }
        context.bundle.resource_templates.emplace(
            resource.resource_stem,
            std::move(resource));
    }

    if (options.legacy_std_json_dir.empty()) {
        if (bchara_dir.has_value()) {
            load_direct_damage_resource(
                context,
                *bchara_dir,
                disc_source_identity_root);
        }
    } else {
        load_legacy_damage_resource(
            context,
            options.legacy_std_json_dir);
    }
    if (!context.bundle.target_reaction_effect_resource.has_value()) {
        add_diagnostic(
            context,
            BattlePredictorResourceInputStatus::MissingInput,
            BattlePredictorResourceDiagnosticSeverity::Error,
            "damage.target_reaction_std",
            "required damage.std target-reaction resource is unavailable");
    }

    context.bundle.bundle_digest = compute_bundle_digest(context.bundle);
    auto bundle = std::make_shared<const BattlePredictorResourceBundle>(
        std::move(context.bundle));
    return SpiceBattleContentAdapterResult{
        .bundle = bundle,
        .status = bundle->status,
        .diagnostics = bundle->diagnostics,
    };
}

const char* spice_battle_content_adapter_rule_detail() {
    return "Builds one immutable first-battle predictor resource bundle from "
           "the pinned in-process SPICE StdFile and canonical MldFile parsers. "
           "The direct provider is the default; an explicitly supplied legacy "
           "STD JSON directory affects only STD projection, while MLD motion "
           "catalogs always come directly from canonical MldFile bindings and "
           "Motion::declared_frame_count. Inputs are resolved case-insensitively "
           "with case collisions classified Ambiguous, and the bundle digest is "
           "path-independent over normalized logical source identities.";
}

} // namespace savor::predict
