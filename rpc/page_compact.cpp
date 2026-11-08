#include "page_measure.h"

namespace pkv {
const size_t page_batch::max_expanded_records;
const size_t page_batch::max_expanded_bytes;
const size_t page_batch::max_erase_ids;
const size_t page_batch::group_storage_bytes;

size_t page_batch::erase_run(const std::vector<page_change>& pages, size_t at) {
    if (at >= pages.size()) return 0;
    const auto& first = pages[at];
    if (first.action != db::update_action::erase) return 1;
    size_t count = 1;
    while (count < max_erase_ids && count < pages.size() - at &&
        pages[at + count].owner == first.owner &&
        pages[at + count].action == db::update_action::erase) ++count;
    return count;
}

bool page_batch::measure_compact(unsigned database,
    const std::vector<db::batch_entry>& roots,
    const std::vector<page_change>& pages, page_batch_size& out) {
    if (database >= max_databases || roots.size() > max_records - 2 ||
        pages.size() > max_expanded_records - 2) return false;
    page_batch_size size;
    size.bytes = 56; size.records = size.actions = 2;
    size.expanded_bytes = sizeof(page_batch); size.compact = true;
    size_t cursor = 0;
    const db::batch_entry* previous = nullptr;
    for (const auto& root : roots) {
        if (!page_measure::valid_root(root, previous)) return false;
        previous = &root;
        if (cursor < pages.size() && pages[cursor].owner < root.key)
            return false;
        if (root.action == db::update_action::keep &&
            (cursor == pages.size() || pages[cursor].owner != root.key))
            continue;
        const auto value = root.action == db::update_action::write ?
            root.value.size() : 0;
        if (!page_measure::charge(size, root.key.size(), value) ||
            !page_measure::bounded_storage(size, sizeof(db::batch_entry),
                root.key.size(), value)) return false;
        ++size.roots;
        uint64_t id = 0;
        size_t remaining = 0;
        while (cursor < pages.size() && pages[cursor].owner == root.key) {
            const auto& page = pages[cursor];
            if (!page_measure::valid_page(root, page, id) ||
                !page_measure::bounded_storage(size, sizeof(page_change),
                    page.owner.size(), page.value.size())) return false;
            if (!remaining) {
                remaining = erase_run(pages, cursor);
                const auto bytes = remaining > 1 ? 8 * remaining :
                    8 + page.value.size();
                if (!page_measure::charge(size, page.owner.size(), bytes))
                    return false;
            }
            --remaining; ++cursor; ++size.pages; id = page.id;
        }
    }
    if (cursor != pages.size() || !size.roots) return false;
    out = size; return true;
}
} // namespace pkv
