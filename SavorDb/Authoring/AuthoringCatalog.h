#pragma once

#include <string_view>

#include "../../SavorCore/Runner/Runtime/Predicates/PredicateExecution.h"

namespace savor::db::authoring::catalog {

struct PredicateSemanticInputToken { std::string_view role_id; };
struct PredicateValueToken { std::string_view recipe_id; };
struct PredicateHookToken { std::string_view canonical_id; };
struct WorkflowUnitToken { std::string_view unit_kind; };
struct WorkflowPortToken { std::string_view key; };

namespace types {
inline const auto U16 = runtime::program::TypeRef::Builtin(
    runtime::program::BuiltinType::U16);
inline const auto U32 = runtime::program::TypeRef::Builtin(
    runtime::program::BuiltinType::U32);
}

namespace predicate::inputs::battle {
inline constexpr PredicateSemanticInputToken TurnInputs{"battle.turn_inputs"};
inline constexpr PredicateSemanticInputToken TurnOrder{"battle.turn_order"};
inline constexpr PredicateSemanticInputToken Rewards{"battle.rewards"};
}

namespace predicate::values::battle {
inline constexpr PredicateValueToken CurrentTurn{"battle.current_turn"};
inline constexpr PredicateValueToken InventoryCount{
    "battle.turn_inputs.inventory_count"};
inline constexpr PredicateValueToken PlayerCount{
    "battle.turn_order.player_count"};
inline constexpr PredicateValueToken EnemyCount{
    "battle.turn_order.enemy_count"};
inline constexpr PredicateValueToken PlayerMinPosition{
    "battle.turn_order.player_min_position"};
inline constexpr PredicateValueToken PlayerMaxPosition{
    "battle.turn_order.player_max_position"};
inline constexpr PredicateValueToken EnemyMinPosition{
    "battle.turn_order.enemy_min_position"};
inline constexpr PredicateValueToken EnemyMaxPosition{
    "battle.turn_order.enemy_max_position"};
inline constexpr PredicateValueToken DropCount{"battle.rewards.drop_count"};
}

namespace predicate::hooks::battle {
inline constexpr PredicateHookToken TurnInputs{
    "builtin.bp.battle.TurnInputs"};
inline constexpr PredicateHookToken TurnIsReady{
    "builtin.bp.battle.TurnIsReady"};
inline constexpr PredicateHookToken EndTurn{"builtin.bp.battle.EndTurn"};
inline constexpr PredicateHookToken EndBattleVictory{
    "builtin.bp.battle.EndBattleVictory"};
}

namespace workflow::units {
inline constexpr WorkflowUnitToken TasMovieEstablishRootCursor{
    "tas_movie_establish_root_cursor"};
inline constexpr WorkflowUnitToken TasMovieValidateRoot{
    "tas_movie_validate_root"};
inline constexpr WorkflowUnitToken TasMovieCheckpointSterilize{
    "tas_movie_checkpoint_sterilize"};
inline constexpr WorkflowUnitToken TasMovieAnnotate{
    "tas_movie_annotate"};
inline constexpr WorkflowUnitToken TasMovieRevise{
    "tas_movie_revise"};
inline constexpr WorkflowUnitToken TasMovieFirstBattleExploration{
    "tas_movie_first_battle_exploration"};
inline constexpr WorkflowUnitToken TasMovieDelayExploration{
    "tas_movie_delay_exploration"};
inline constexpr WorkflowUnitToken SeedProbe{"seed_probe"};
inline constexpr WorkflowUnitToken BattleContext{"battle.context"};
inline constexpr WorkflowUnitToken Battle{"battle"};
}

namespace workflow::ports::tas_movie_establish_root_cursor {
inline constexpr WorkflowPortToken RootDtm{"root_dtm"};
inline constexpr WorkflowPortToken ValidationAttempt{
    "tas_movie_validation_attempt"};
inline constexpr WorkflowPortToken EstablishedRootCursorAttempt{
    "established_root_cursor_attempt"};
}
namespace workflow::ports::tas_movie_validate_root {
inline constexpr WorkflowPortToken RootEstablishment{"root_establishment"};
inline constexpr WorkflowPortToken ValidationAttempt{
    "tas_movie_validation_attempt"};
inline constexpr WorkflowPortToken ValidatedCheckpoint{
    "validated_checkpoint_savestate"};
}
namespace workflow::ports::tas_movie_checkpoint_sterilize {
inline constexpr WorkflowPortToken PairedCheckpoint{
    "paired_checkpoint_savestate"};
inline constexpr WorkflowPortToken SterilizedCheckpoint{
    "sterilized_checkpoint_savestate"};
}
namespace workflow::ports::tas_movie_annotate {
inline constexpr WorkflowPortToken RootDtm{"root_dtm"};
inline constexpr WorkflowPortToken AnnotationAttempt{"annotation_attempt"};
}
namespace workflow::ports::tas_movie_revise {
inline constexpr WorkflowPortToken AnnotationAttempt{"annotation_attempt"};
inline constexpr WorkflowPortToken RewriteAttempt{"rewrite_attempt"};
inline constexpr WorkflowPortToken RewrittenDtm{"rewritten_dtm"};
inline constexpr WorkflowPortToken RewrittenPairedSavestate{
    "rewritten_paired_savestate"};
}
namespace workflow::ports::tas_movie_first_battle_exploration {
inline constexpr WorkflowPortToken RootDtm{"root_dtm"};
}
namespace workflow::ports::tas_movie_delay_exploration {
inline constexpr WorkflowPortToken TasNode{"tas_node"};
}
namespace workflow::ports::seed_probe {
inline constexpr WorkflowPortToken EntrySavestate{"entry_savestate"};
inline constexpr WorkflowPortToken Run{"seed_probe_run"};
}
namespace workflow::ports::battle_context {
inline constexpr WorkflowPortToken EntrySavestate{"entry_savestate"};
inline constexpr WorkflowPortToken Context{"battle_context"};
}
namespace workflow::ports::battle {
inline constexpr WorkflowPortToken SeedProbeRun{"seed_probe_run"};
inline constexpr WorkflowPortToken Context{"battle_context"};
inline constexpr WorkflowPortToken BattleSet{"battle_set"};
inline constexpr WorkflowPortToken ManualFollowup{"battle_manual_followup"};
}

namespace predicate::expression {
using Node = runtime::predicates::PredicateGuidedNodeV1;
using Kind = runtime::predicates::PredicateGuidedNodeKindV1;

inline Node SemanticInput(const PredicateSemanticInputToken token)
{
    return {.kind = Kind::SemanticInput, .key = std::string(token.role_id)};
}

inline Node Parameter(std::string name, runtime::program::TypeRef type)
{
    return {.kind = Kind::Parameter, .key = std::move(name),
            .declared_type = std::move(type)};
}

inline Node Value(const PredicateValueToken token, std::vector<Node> arguments = {})
{
    return {.kind = Kind::CatalogValue, .key = std::string(token.recipe_id),
            .children = std::move(arguments)};
}

inline Node Equal(Node left, Node right)
{
    return {.kind = Kind::Equal,
            .children = {std::move(left), std::move(right)}};
}

inline Node NotEqual(Node left, Node right)
{
    return {.kind = Kind::NotEqual,
            .children = {std::move(left), std::move(right)}};
}

inline Node Less(Node left, Node right)
{
    return {.kind = Kind::Less,
            .children = {std::move(left), std::move(right)}};
}

inline Node LessEqual(Node left, Node right)
{
    return {.kind = Kind::LessEqual,
            .children = {std::move(left), std::move(right)}};
}

inline Node Greater(Node left, Node right)
{
    return {.kind = Kind::Greater,
            .children = {std::move(left), std::move(right)}};
}

inline Node GreaterEqual(Node left, Node right)
{
    return {.kind = Kind::GreaterEqual,
            .children = {std::move(left), std::move(right)}};
}
}

} // namespace savor::db::authoring::catalog
