#include "stdafx.h"
#ifdef HAS_WT
#include "wt.h"
#include "wt_sess.h"
#include "db/disk_address.h"
#include <thread>

namespace pkv {

wt_sess::wt_sess(wt& database) : database_(database) {}
wt_sess::~wt_sess() {
    if (transaction_) session_->rollback_transaction(session_, nullptr);
    for (auto* cursor : {roots_, pages_, end_, index_})
        if (cursor) cursor->close(cursor);
    if (session_) session_->close(session_, nullptr);
}
bool wt_sess::open() {
    auto* connection = database_.get_db();
    int result = connection->open_session(connection, nullptr, nullptr,
        &session_);
    if (!result) result = session_->open_cursor(session_, "table:pkv_roots_v1",
        nullptr, "overwrite=true", &roots_);
    if (!result) result = session_->open_cursor(session_, "table:pkv_pages_v1",
        nullptr, "overwrite=true", &pages_);
    if (!result) result = session_->open_cursor(session_, "table:pkv_pages_v1",
        nullptr, "overwrite=true", &end_);
    if (!result) result = session_->open_cursor(session_, "table:pkv_scan_v1",
        nullptr, "overwrite=true", &index_);
    if (!result && (std::strcmp(roots_->key_format, "u") ||
        std::strcmp(roots_->value_format, "u"))) result = EINVAL;
    return result == 0;
}
bool wt_sess::count_roots(uint64_t& out) {
    uint64_t count = 0;
    int result;
    roots_->reset(roots_);
    while ((result = roots_->next(roots_)) == 0) {
        if (count == UINT64_MAX) { roots_->reset(roots_); return false; }
        ++count;
    }
    roots_->reset(roots_);
    if (result != WT_NOTFOUND) return false;
    out = count; return true;
}
void wt_sess::reset() {
    if (transaction_) {
        session_->rollback_transaction(session_, nullptr); transaction_ = false;
    }
    for (auto* cursor : {roots_, pages_, end_, index_})
        if (cursor) cursor->reset(cursor);
}
void wt_sess::key(WT_CURSOR* cursor, const std::string& value) {
    WT_ITEM item = {};
    item.data = value.data(); item.size = value.size();
    cursor->set_key(cursor, &item);
}
int wt_sess::put(WT_CURSOR* cursor, const std::string& name,
    const std::string& value) {
    key(cursor, name);
    WT_ITEM item = {};
    item.data = value.data(); item.size = value.size();
    cursor->set_value(cursor, &item);
    const int result = cursor->insert(cursor);
    cursor->reset(cursor);
    return result;
}
int wt_sess::remove(WT_CURSOR* cursor, const std::string& name) {
    key(cursor, name);
    const int result = cursor->remove(cursor);
    cursor->reset(cursor);
    return result == WT_NOTFOUND ? 0 : result;
}
int wt_sess::value(WT_CURSOR* cursor, std::string& out, bool is_key) {
    WT_ITEM item = {};
    const int result = is_key ? cursor->get_key(cursor, &item) :
        cursor->get_value(cursor, &item);
    if (result) return result;
    if (item.size) out.assign(static_cast<const char*>(item.data), item.size);
    else out.clear();
    return 0;
}
int wt_sess::seek(WT_CURSOR* cursor, const std::string& prefix) {
    key(cursor, prefix);
    int comparison = 0;
    const int result = cursor->search_near(cursor, &comparison);
    return result == 0 && comparison < 0 ? cursor->next(cursor) : result;
}

db::lookup_status wt_sess::lookup(bool page, const std::string& name,
    std::string& out) {
    auto* cursor = page ? pages_ : roots_;
    key(cursor, name);
    const int found = cursor->search(cursor);
    int result = found;
    std::string bytes;
    if (!result) result = value(cursor, bytes);
    cursor->reset(cursor);
    if (!result) { out.swap(bytes); return db::lookup_status::found; }
    return found == WT_NOTFOUND ? db::lookup_status::missing :
        db::lookup_status::error;
}
int wt_sess::clear(const std::string& owner) {
    const auto first = disk_address::page(owner, 0);
    const auto last = disk_address::page(owner, UINT64_MAX);
    key(pages_, first);
    key(end_, last);
    return session_->truncate(session_, nullptr, pages_, end_, nullptr);
}
int wt_sess::stage(const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages, bool preserve) {
    for (const auto& root : roots) {
        if (root.action == db::update_action::keep) continue;
        int result = 0;
        if (!preserve || root.clear_pages ||
            root.action == db::update_action::erase) result = clear(root.key);
        if (result) return result;
        const auto address = std::string(1, '\1') + root.key;
        if (root.action == db::update_action::write) {
            result = put(roots_, address, root.value);
            if (!result && !root.found) result = put(index_,
                disk_address::index(root.key), "");
        } else {
            result = remove(roots_, address);
            if (!result) result = remove(index_, disk_address::index(root.key));
        }
        if (result) return result;
    }
    for (const auto& page : pages) {
        const auto address = disk_address::page(page.owner, page.id);
        const int result = page.action == db::update_action::write ?
            put(pages_, address, page.value) : remove(pages_, address);
        if (result) return result;
    }
    return 0;
}

db::batch_status wt_sess::apply(const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages, bool preserve) {
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        reset();
        int result = session_->begin_transaction(session_,
            "isolation=snapshot");
        if (!result) {
            transaction_ = true;
            result = stage(roots, pages, preserve);
            if (!result) {
                result = session_->commit_transaction(session_, "sync=on");
                transaction_ = false;
            }
        }
        if (!result) { reset(); return db::batch_status::ok; }
        reset();
        if (result != WT_ROLLBACK) {
            logger_error("WiredTiger commit failed: %s",
                wiredtiger_strerror(result));
            return db::batch_status::error;
        }
        std::this_thread::yield();
    }
    return db::batch_status::error;
}

db::scan_status wt_sess::scan(unsigned bucket, std::vector<std::string>& out) {
    const auto prefix = std::string(1, static_cast<char>(bucket));
    int result = seek(index_, prefix);
    while (!result) {
        std::string name;
        result = value(index_, name, true);
        if (result || name.empty()) {
            index_->reset(index_); return db::scan_status::error;
        }
        if (static_cast<unsigned char>(name[0]) != bucket) break;
        out.emplace_back(name, 1, std::string::npos);
        result = index_->next(index_);
    }
    index_->reset(index_);
    return !result || result == WT_NOTFOUND ? db::scan_status::ok :
        db::scan_status::error;
}

db::scan_status wt_sess::visit_keys(const db::key_visitor& visit) {
    index_->reset(index_);
    int result;
    while ((result = index_->next(index_)) == 0) {
        std::string name;
        result = value(index_, name, true);
        if (result || name.empty()) {
            index_->reset(index_); return db::scan_status::error;
        }
        const std::string key(name, 1, std::string::npos);
        if (!visit(key)) break;
    }
    index_->reset(index_);
    return !result || result == WT_NOTFOUND ? db::scan_status::ok :
        db::scan_status::error;
}

} // namespace pkv
#endif
