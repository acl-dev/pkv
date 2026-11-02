#include "exec_batch.h"
#include "move_intent.h"
#include <new>

namespace pkv {
std::shared_ptr<const exec_batch> exec_batch::capture_ordered(
    std::vector<exec_entry> entries) {
    if (entries.empty()) return nullptr;
    try {
        std::shared_ptr<exec_batch> out(new exec_batch);
        size_t staged_bytes = 0, staged_records = 0;
        bool moves = false;
        for (const auto& entry : entries) {
            size_t bytes, records, charge, count;
            if (entry.pages()) {
                const auto& size = entry.pages()->size();
                bytes = charge = size.bytes; records = count = size.records;
                out->batches_.push_back(entry.pages());
            } else if (entry.move()) {
                moves = true;
                const auto& move = *entry.move();
                if (!move_intent::measure(move, bytes)) return nullptr;
                records = 1; count = 3;
                charge = 64 + 2 * move.key.size() + move.source_root.size() +
                    move.target_root.size();
            } else return nullptr;
            if (bytes > page_batch::max_bytes - out->bytes_ ||
                records > page_batch::max_records - out->records_ ||
                charge > page_batch::max_bytes - staged_bytes ||
                count > page_batch::max_records - staged_records)
                return nullptr;
            out->bytes_ += bytes; out->records_ += records;
            staged_bytes += charge; staged_records += count;
        }
        // Preserve the old page-only representation and minimum version.
        if (moves) out->entries_ = std::move(entries);
        return out;
    } catch (const std::bad_alloc&) { return nullptr; }
}
bool exec_batch::has_moves() const {
    for (const auto& entry : entries_) if (entry.move()) return true;
    return false;
}
bool exec_batch::touches(unsigned database) const {
    for (const auto& batch : batches_)
        if (batch->database() == database) return true;
    for (const auto& entry : entries_) if (entry.move() &&
        (entry.move()->source == database || entry.move()->target == database))
        return true;
    return false;
}
} // namespace pkv
