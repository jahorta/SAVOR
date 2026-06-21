#include "ProgressEventParser.h"

#include <regex>

namespace savor::predict {

namespace {

std::string trim(std::string_view value) {
    auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string progress_payload(const std::string& message) {
    const auto marker = message.find("progress=");
    if (marker == std::string::npos) {
        return message;
    }
    return message.substr(marker + 9);
}

} // namespace

PlannedAction parse_planned_action(std::string_view text) {
    const auto cleaned = trim(text);
    PlannedAction action;
    if (cleaned.rfind("Attack->", 0) == 0) {
        action.kind = PlannedActionKind::Attack;
        action.target = cleaned.substr(8);
    } else if (cleaned == "Defend" || cleaned == "Guard") {
        action.kind = PlannedActionKind::Defend;
    }
    return action;
}

std::string planned_action_to_string(const PlannedAction& action) {
    switch (action.kind) {
    case PlannedActionKind::Attack:
        return "Attack->" + action.target;
    case PlannedActionKind::Defend:
        return "Defend";
    case PlannedActionKind::Unknown:
    default:
        return "Unknown";
    }
}

ParsedProgressEvents parse_progress_events(const std::vector<std::string>& messages) {
    ParsedProgressEvents parsed;

    const std::regex plan_regex(
        R"(Vyse: ([^\r\n]+)\r?\nAika: ([^\r\n]+)\r?\n\[4\]Soldier: ([^\r\n]+)\r?\n\[5\]Soldier: ([^\r\n]+))");
    const std::regex predicate_regex(R"(PCs act before ECs - ([0-9]+) < ([0-9]+))");
    const std::regex attack_regex(R"((Vyse|Aika|\[[0-9]+\]Soldier) attacks (.+?) for ([0-9]+) damage)");
    const std::regex counter_regex(R"((Vyse|Aika|\[[0-9]+\]Soldier) counter attacks)");
    const std::regex death_regex(R"((\[[0-9]+\]Soldier) died)");
    const std::regex drop_regex(R"((\[[0-9]+\]Soldier) dropped (.+))");

    std::optional<AttackEvent> previous_attack;
    for (const auto& message : messages) {
        const auto payload = progress_payload(message);
        std::smatch match;

        if (!parsed.planned_actions && std::regex_search(payload, match, plan_regex)) {
            PlannedTurnActions actions;
            actions.vyse = parse_planned_action(match[1].str());
            actions.aika = parse_planned_action(match[2].str());
            actions.soldier4 = parse_planned_action(match[3].str());
            actions.soldier5 = parse_planned_action(match[4].str());
            parsed.planned_actions = actions;
            continue;
        }

        if (!parsed.pc_before_ec_predicate && std::regex_search(payload, match, predicate_regex)) {
            parsed.pc_before_ec_predicate = PcBeforeEcPredicate{
                std::stoi(match[1].str()),
                std::stoi(match[2].str())};
            continue;
        }

        if (std::regex_search(payload, match, attack_regex)) {
            AttackEvent attack{match[1].str(), match[2].str(), std::stoi(match[3].str())};
            parsed.attacks.push_back(attack);
            parsed.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Attack, attack, {}, {}, {}});
            previous_attack = attack;
            continue;
        }

        if (std::regex_search(payload, match, counter_regex)) {
            CounterEvent counter;
            counter.actor = match[1].str();
            if (previous_attack.has_value() && previous_attack->target == counter.actor) {
                counter.target = previous_attack->actor;
                counter.target_inferred = true;
            }
            parsed.counters.push_back(counter);
            parsed.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Counter, {}, counter, {}, {}});
            continue;
        }

        if (std::regex_search(payload, match, death_regex)) {
            const auto target = match[1].str();
            parsed.deaths.push_back(target);
            parsed.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Death, {}, {}, target, {}});
            continue;
        }

        if (std::regex_search(payload, match, drop_regex)) {
            DropEvent drop{match[1].str(), match[2].str()};
            parsed.drops.push_back(drop);
            parsed.ordered_combat_events.push_back(CombatEvent{CombatEventKind::Drop, {}, {}, drop.target, drop.drop});
            continue;
        }
    }

    return parsed;
}

} // namespace savor::predict
