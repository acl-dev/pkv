#pragma once
#include "family_plan.h"
#include <map>
#include <mutex>

namespace pkv {
struct family_plan::state {
    struct database {
        shared_db value;
        std::vector<db::batch_entry> roots;
        std::vector<key_ready_change> ready;
        std::vector<size_t> ranks;
        // Native frames borrow even their empty page input until destruction.
        std::vector<page_change> pages;
        std::unique_ptr<db::prepared_batch> frame;
    };
    std::vector<std::unique_lock<acl::fiber_mutex>> guards;
    // Keep DBs alive until all detached maps have restored or been consumed.
    std::vector<shared_db> pins;
    std::vector<std::unique_ptr<db::prepared_pages>> originals, fresh;
    // Frames must be destroyed BEFORE originals/fresh on every error path.
    std::map<unsigned, database> databases;
    std::vector<database*> ordered;
    std::vector<db::prepared_pages*> families;
    std::vector<std::unique_ptr<db::prepared_transfer>> transfers;
};
} // namespace pkv
