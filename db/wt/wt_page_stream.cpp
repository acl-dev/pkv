#include "stdafx.h"
#ifdef HAS_WT
#include "wt_sess.h"
#include "db/disk_address.h"
#include "rpc/page_wire.h"

namespace pkv {
db::lookup_status wt_sess::bounded_root(const std::string& owner,
    std::string& out) {
    if (!session_ || !roots_ || transaction_) return db::lookup_status::error;
    key(roots_, std::string(1, '\1') + owner);
    const int found = roots_->search(roots_);
    WT_ITEM item = {};
    const int result = found ? found : roots_->get_value(roots_, &item);
    if (result || item.size > page_transaction::max_value_bytes) {
        roots_->reset(roots_);
        return found == WT_NOTFOUND ? db::lookup_status::missing :
            db::lookup_status::error;
    }
    try {
        std::string bytes(item.size ? static_cast<const char*>(item.data) :
            "", item.size);
        roots_->reset(roots_); out.swap(bytes);
        return db::lookup_status::found;
    } catch (...) { roots_->reset(roots_); throw; }
}
db::batch_status wt_sess::visit_pages(const std::string& owner,
    const db::page_visitor& visit) {
    if (!session_ || !pages_ || transaction_) return db::batch_status::error;
    struct reset_cursor {
        WT_CURSOR* cursor;
        ~reset_cursor() { cursor->reset(cursor); }
    } reset{pages_};
    const auto prefix = disk_address::prefix(owner);
    int result = seek(pages_, prefix);
    while (!result) {
        WT_ITEM address = {}, value = {};
        result = pages_->get_key(pages_, &address);
        if (result) return db::batch_status::error;
        if (address.size < prefix.size() || std::memcmp(address.data,
            prefix.data(), prefix.size())) break;
        if (address.size != prefix.size() + 8) return db::batch_status::error;
        const auto id = page_wire::get64(
            static_cast<const char*>(address.data) + prefix.size());
        result = pages_->get_value(pages_, &value);
        if (result) return db::batch_status::error;
        if (!id || value.size > page_transaction::max_value_bytes)
            return db::batch_status::error;
        const std::string bytes(value.size ?
            static_cast<const char*>(value.data) : "", value.size);
        if (!visit(id, bytes)) return db::batch_status::aborted;
        result = pages_->next(pages_);
    }
    return !result || result == WT_NOTFOUND ? db::batch_status::ok :
        db::batch_status::error;
}
} // namespace pkv
#endif
