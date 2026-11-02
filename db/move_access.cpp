#include "stdafx.h"
#include "move_access.h"
#include "database_pool.h"
#include "page_transaction.h"

namespace pkv {
move_access::move_access(database_pool& pool, size_t source, size_t target,
    const std::string& owner, kind mode) : admission_(true) {
    if (source >= pool.size() || target >= pool.size() || source == target)
        return;
    source_ = pool.get(source); target_ = pool.get(target);
    if (!source_ || !target_ || source_ == target_) return;
    if (mode == kind::memory && (!source_->dbsupports_pages() ||
        !target_->dbsupports_pages() ||
        !source_->dbsupports_family_transfer(*target_) ||
        !target_->dbsupports_family_transfer(*source_) ||
        !source_->dbsupports_prepare() || !target_->dbsupports_prepare())) {
        status_ = db::batch_status::unsupported; return;
    }
    if (!source_->opened_ || !target_->opened_) return;
    if (mode == kind::disk) {
        for (const auto& database : {source_, target_})
            if (!database->dbsupports_pages() || !database->dbsupports_sync() ||
                !database->dbsupports_page_stream()) {
                status_ = db::batch_status::unsupported; return;
            }
    }
    const bool from_first = std::less<db*>()(source_.get(), target_.get());
    auto& one = (from_first ? source_ : target_)->key_mutex(owner);
    auto& two = (from_first ? target_ : source_)->key_mutex(owner);
    if (!one.lock()) return;
    first_ = std::unique_lock<acl::fiber_mutex>(one, std::adopt_lock);
    if (!two.lock()) return;
    second_ = std::unique_lock<acl::fiber_mutex>(two, std::adopt_lock);
    status_ = db::batch_status::ok;
}
bool move_access::load(const std::string& owner) {
    from_status_ = source_->dblookup_bounded_root(owner, from_);
    to_status_ = target_->dblookup_bounded_root(owner, to_);
    return from_status_ != db::lookup_status::error &&
        to_status_ != db::lookup_status::error &&
        from_.size() <= page_transaction::max_value_bytes &&
        to_.size() <= page_transaction::max_value_bytes;
}
} // namespace pkv
