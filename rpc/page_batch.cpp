#include "page_batch.h"
#include "page_measure.h"
#include "page_wire.h"
#include <new>

namespace pkv {

const size_t page_batch::max_bytes;
const size_t page_batch::max_records;
const size_t page_batch::max_key_bytes;
const unsigned page_batch::max_databases;

using page_measure::charge;

bool page_batch::measure_plain(unsigned database,
    const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages, page_batch_size& out) {
    if (database >= max_databases || roots.size() > max_records - 2 ||
        pages.size() > max_records - 2) {
        return false;
    }
    page_batch_size size;
    // BEGIN has 16 payload bytes; COMMIT has none.
    size.bytes = 2 * page_wire::header_bytes + 16;
    size.records = size.actions = 2;
    size.expanded_bytes = sizeof(page_batch);
    size_t cursor = 0;
    const db::batch_entry* previous = nullptr;
    for (const auto& root : roots) {
        if (!page_measure::valid_root(root, previous)) return false;
        previous = &root;
        if (cursor < pages.size() && pages[cursor].owner < root.key) {
            return false;
        }
        if (root.action == db::update_action::keep &&
            (cursor == pages.size() || pages[cursor].owner != root.key)) {
            continue;
        }
        if (!charge(size, root.key.size(),
            root.action == db::update_action::write ? root.value.size() : 0)) {
            return false;
        }
        ++size.roots;
        page_measure::storage(size, sizeof(db::batch_entry), root.key.size(),
            root.action == db::update_action::write ? root.value.size() : 0);
        uint64_t previous_id = 0;
        while (cursor < pages.size() && pages[cursor].owner == root.key) {
            const auto& page = pages[cursor++];
            if (!page_measure::valid_page(root, page, previous_id) ||
                !charge(size, page.owner.size(), 8 + page.value.size()))
                return false;
            previous_id = page.id;
            ++size.pages;
            page_measure::storage(size, sizeof(page_change),
                page.owner.size(), page.value.size());
        }
    }
    if (cursor != pages.size() || size.roots == 0) {
        return false;
    }
    out = size;
    return true;
}

bool page_batch::measure(unsigned database,
    const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages, page_batch_size& out) {
    return measure_plain(database, roots, pages, out) ||
        measure_compact(database, roots, pages, out);
}

std::shared_ptr<const page_batch> page_batch::capture(unsigned database,
    const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages) {
    page_batch_size size;
    if (!measure(database, roots, pages, size)) {
        return nullptr;
    }
    try {
        auto batch = std::make_shared<page_batch>(construction_key{}, database);
        batch->size_ = size;
        batch->roots_.reserve(size.roots);
        size_t cursor = 0;
        for (const auto& root : roots) {
            while (cursor < pages.size() && pages[cursor].owner < root.key) {
                ++cursor;
            }
            if (root.action == db::update_action::keep &&
                (cursor == pages.size() || pages[cursor].owner != root.key)) {
                continue;
            }
            batch->roots_.emplace_back(root.key, root.found,
                root.action == db::update_action::write ? root.value : "");
            batch->roots_.back().action = root.action;
            batch->roots_.back().clear_pages = root.clear_pages;
            batch->roots_.back().maintenance_only = root.maintenance_only;
        }
        batch->pages_ = pages;
        return batch;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

db::batch_status page_batch::apply(db& destination) const {
    return destination.replica_apply_pages(roots_, pages_);
}

void page_receiver::reset() {
    batch_.reset();
    expected_ = page_batch_size();
    received_bytes_ = received_records_ = expanded_bytes_ = 0;
    failed_ = false; protocol_ = 0;
}

page_receiver::result page_receiver::reject() {
    reset();
    failed_ = true;
    return result::error;
}

bool page_receiver::begin(const page_frame& frame) {
    if (frame.operation != page_operation::begin || !frame.key.empty() ||
        frame.value.size() != 16 || frame.id != 0 ||
        frame.database >= page_batch::max_databases ||
        (frame.protocol < 2 || frame.protocol > 5)) {
        return false;
    }
    const auto roots = page_wire::get32(frame.value.data());
    const auto pages = page_wire::get32(frame.value.data() + 4);
    const auto bytes = page_wire::get64(frame.value.data() + 8);
    if (!roots || roots > page_batch::max_records - 2 ||
        uint64_t(pages) + roots + 2 > (frame.protocol == 5 ?
            action_limit_ : page_batch::max_records) ||
        bytes > page_batch::max_bytes ||
        bytes < 56 + uint64_t(roots) * 20 +
            uint64_t(pages) * (frame.protocol == 5 ? 8 : 28)) {
        return false;
    }
    const auto objects = sizeof(page_batch) +
        size_t(roots) * (sizeof(db::batch_entry) + 64) +
        size_t(pages) * (sizeof(page_change) + 64);
    if (objects > storage_limit_) return false;
    batch_ = std::make_shared<page_batch>(page_batch::construction_key{},
        frame.database);
    expanded_bytes_ = objects;
    protocol_ = frame.protocol;
    expected_.roots = roots;
    expected_.pages = pages;
    expected_.records = frame.protocol == 5 ? 0 : 2 + roots + pages;
    expected_.actions = 2 + roots + pages;
    expected_.bytes = static_cast<size_t>(bytes);
    received_bytes_ = 36; received_records_ = 1;
    batch_->roots_.reserve(roots);
    batch_->pages_.reserve(pages);
    return true;
}

bool page_receiver::append_root(page_frame& frame) {
    auto& roots = batch_->roots_;
    if (!batch_->pages_.empty() || roots.size() >= expected_.roots ||
        frame.id != 0 || (!roots.empty() && roots.back().key >= frame.key)) {
        return false;
    }
    db::update_action action;
    switch (frame.operation) {
    case page_operation::root_keep: action = db::update_action::keep; break;
    case page_operation::root_maintenance:
    case page_operation::root_replace:
    case page_operation::root_write: action = db::update_action::write; break;
    case page_operation::root_erase: action = db::update_action::erase; break;
    default: return false;
    }
    if (frame.value.size() > page_transaction::max_value_bytes ||
        (action != db::update_action::write && !frame.value.empty())) {
        return false;
    }
    if (!storage(frame.key.size(), frame.value.size())) return false;
    roots.emplace_back(std::move(frame.key), true, std::move(frame.value));
    roots.back().action = action;
    roots.back().maintenance_only =
        frame.operation == page_operation::root_maintenance;
    roots.back().clear_pages = roots.back().maintenance_only ||
        frame.operation == page_operation::root_replace;
    return true;
}

bool page_receiver::append_page(page_frame& frame) {
    auto& pages = batch_->pages_;
    if (batch_->roots_.size() != expected_.roots ||
        pages.size() >= expected_.pages || frame.id == 0 ||
        frame.value.size() > page_transaction::max_value_bytes ||
        (frame.operation == page_operation::page_erase &&
            !frame.value.empty())) {
        return false;
    }
    if (!pages.empty() && (frame.key < pages.back().owner ||
        (frame.key == pages.back().owner && frame.id <= pages.back().id))) {
        return false;
    }
    const auto& roots = batch_->roots_;
    const auto root = std::lower_bound(roots.begin(), roots.end(), frame.key,
        [](const db::batch_entry& entry, const std::string& name) {
            return entry.key < name;
        });
    if (root == roots.end() || root->key != frame.key ||
        (root->action == db::update_action::erase &&
            frame.operation == page_operation::page_write)) {
        return false;
    }
    if (!storage(frame.key.size(), frame.value.size())) return false;
    pages.emplace_back(std::move(frame.key), frame.id, std::move(frame.value),
        frame.operation == page_operation::page_write ?
            db::update_action::write : db::update_action::erase);
    return true;
}

bool page_receiver::finish() {
    page_batch_size actual;
    const bool measured = protocol_ == 5 ? page_batch::measure_compact(
        batch_->database_, batch_->roots_, batch_->pages_, actual) :
        page_batch::measure(batch_->database_, batch_->roots_, batch_->pages_,
            actual);
    if (!measured || batch_->roots_.size() != expected_.roots ||
        batch_->pages_.size() != expected_.pages ||
        received_bytes_ + 20 != expected_.bytes ||
        actual.bytes != expected_.bytes || actual.roots != expected_.roots ||
        actual.records != received_records_ + 1 ||
        actual.expanded_bytes != expanded_bytes_) return false;
    expected_ = actual; return true;
}

page_receiver::result page_receiver::accept(page_frame frame,
    std::shared_ptr<const page_batch>& out, size_t storage_limit,
    size_t action_limit) {
    out.reset();
    if (failed_) {
        return result::error;
    }
    try {
        if (!batch_) {
            storage_limit_ = std::min(storage_limit,
                page_batch::max_expanded_bytes);
            action_limit_ = std::min(action_limit,
                page_batch::max_expanded_records);
            return begin(frame) ? result::pending : reject();
        }
        if (frame.protocol != protocol_ ||
            frame.database != batch_->database_ ||
            frame.key.size() > page_batch::max_key_bytes) {
            return reject();
        }
        if (frame.operation == page_operation::commit) {
            if (!frame.key.empty() || !frame.value.empty() || frame.id ||
                !finish()) {
                return reject();
            }
            batch_->size_ = expected_;
            out = batch_;
            reset();
            return result::complete;
        }
        const bool page = frame.operation == page_operation::page_write ||
            frame.operation == page_operation::page_erase;
        const size_t bytes = 20 + frame.key.size() + frame.value.size() +
            (page ? 8 : 0);
        const bool group = frame.operation ==
            page_operation::page_erase_group;
        if (received_records_ >= page_batch::max_records - 1 ||
            bytes > expected_.bytes - received_bytes_ ||
            !(group ? append_erase_group(frame) : page ? append_page(frame) :
                append_root(frame))) {
            return reject();
        }
        received_bytes_ += bytes; ++received_records_;
        return result::pending;
    } catch (const std::bad_alloc&) {
        return reject();
    }
}

} // namespace pkv
