#pragma once

namespace pkv {
// Prepared before a native MOVE. Destruction cancels uncommitted ownership.
// Observers outlive the object; retain exclusive admission until resolution.
class family_publication {
public:
    virtual ~family_publication() = default;
    virtual void publish() noexcept = 0;
};
} // namespace pkv
