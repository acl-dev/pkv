#pragma once
#include "db.h"
#include <utility>

namespace pkv {
// Only this cold handle owns the transfer pointers. Ordinary commit frames
// gain no per-key fields or scratch buffers. Destruction never dereferences
// the maps: either native batch may erase the empty source after publication.
template<class Map> class prepared_family : public db::prepared_transfer {
    Map* source_;
    Map* target_;
public:
    prepared_family(Map* source, Map* target)
    : source_(source), target_(target) {
        static_assert(noexcept(std::declval<Map&>().swap(
            std::declval<Map&>())), "family swap must not throw");
    }
    void publish() noexcept override {
        if (source_) source_->swap(*target_);
    }
};
} // namespace pkv
