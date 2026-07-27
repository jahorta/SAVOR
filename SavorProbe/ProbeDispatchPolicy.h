#pragma once

#include "ProbeProfile.h"

namespace savor::probe {

struct SubscriberDispatchDecision {
    bool capture = false;
    bool progress = false;
    bool control = false;
};

constexpr SubscriberDispatchDecision subscriber_dispatch_decision(
    Subscription subscriptions,
    bool capture_enabled,
    bool active_foreground_wake,
    bool control_already_published = false)
{
    return {
        .capture = capture_enabled
            && (has_subscription(subscriptions, Subscription::Capture)
                || has_subscription(subscriptions, Subscription::Progress)),
        .progress = has_subscription(subscriptions, Subscription::Progress),
        .control = active_foreground_wake && !control_already_published
            && has_subscription(subscriptions, Subscription::Control),
    };
}

} // namespace savor::probe
