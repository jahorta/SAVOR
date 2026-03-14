#include "DropInbox.h"

void DropInbox::Push(DropEvent ev) {
    std::lock_guard lk(mtx_);
    q_.push(std::move(ev));
}

bool DropInbox::TryPop(DropEvent& out) {
    std::lock_guard lk(mtx_);
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop();
    return true;
}

void DropInbox::Clear() {
    DropEvent out;
    while (TryPop(out))
        continue;
}
