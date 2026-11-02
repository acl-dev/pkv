#include "db.h"
#include "page_transaction.h"
#include "family_move.h"
#include "family_publication.h"
#include "common/key_wait.h"
#include <new>

namespace pkv {
struct db::move_effects : family_publication {
    db& source;
    db& target;
    std::vector<batch_entry> from, to;
    std::vector<key_ready_change> from_ready, to_ready;
    bool published = false;
    move_effects(db& source_db, db& target_db, const family_move& move)
    : source(source_db), target(target_db) {
        from.emplace_back(move.key, true, "");
        from.front().action = update_action::erase;
        to.emplace_back(move.key, move.target_found, move.source_root);
        to.front().action = update_action::write;
        to.front().clear_pages = true;
        from_ready = source.prepare_ready(from);
        to_ready = target.prepare_ready(to);
        if (source.ready_registry_)
            source.ready_registry_->prepare(from_ready.data(),
                from_ready.size());
        if (target.ready_registry_)
            target.ready_registry_->prepare(to_ready.data(), to_ready.size());
    }
    void publish() noexcept override {
        if (published) return;
        source.publish_page_effects(from, {}, from_ready);
        target.publish_page_effects(to, {}, to_ready);
        published = true;
    }
};
bool db::prepare_move_effects(db& target, const family_move& move,
    std::unique_ptr<family_publication>& out) {
    out.reset();
    try { out.reset(new move_effects(*this, target, move)); return true; }
    catch (const std::bad_alloc&) { return false; }
}
} // namespace pkv
