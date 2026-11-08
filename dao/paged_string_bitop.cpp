#include "stdafx.h"
#include "paged_string.h"
#include "string_bitop_source.h"

namespace pkv {
namespace dao {

bool paged_string::bitop(shared_db& db, bitmap_operation operation,
    const std::vector<std::string>& sources, const std::string& destination,
    long long& length) {
    reset(); length = 0;
    if (!valid_bitop(operation, sources.size())) {
        read_status_ = read_status::invalid_record; return false;
    }
    auto locked = sources;
    locked.push_back(destination);
    const auto status = db->transact_pages(std::move(locked),
        [&](page_transaction& tx) {
        const auto now = unix_time_ms();
        const auto roots = tx.roots();
        std::vector<string_bitop_source> distinct;
        distinct.reserve(roots.size());
        std::vector<string_bitop_source*> by_root(roots.size(), nullptr);
        std::vector<string_bitop_source*> inputs;
        inputs.reserve(sources.size());
        uint64_t size = 0;
        for (const auto& key : sources) {
            const auto* root = roots.find(key);
            auto*& source = by_root[root - roots.begin()];
            if (!source) {
                // Each root has one reader, but each argument still folds
                // independently. Repeated XOR and ONE inputs can cancel.
                distinct.emplace_back(db, tx, *root, now);
                source = &distinct.back();
                if (!source->open()) {
                    read_status_ = source->error(); return false;
                }
                size = std::max(size, source->length());
            }
            inputs.push_back(source);
        }
        if (!size) {
            auto& entry = *roots.find(destination);
            if (entry.found) entry.action = db::update_action::erase;
            return true;
        }
        string_store target(tx, destination);
        const bool uses_state = bitop_uses_state(operation);
        std::string state, input;
        const auto produce = [&](uint64_t offset, size_t count,
            std::string& out) {
            if (uses_state) state.assign(count, '\0');
            auto* mask = uses_state ? &state[0] : nullptr;
            if (!inputs.front()->read(offset, count, out)) {
                read_status_ = inputs.front()->error(); return false;
            }
            out.resize(count, '\0');
            for (size_t i = 1; i < inputs.size(); ++i) {
                if (!inputs[i]->fold(offset, count, operation, &out[0],
                    mask, input)) {
                    read_status_ = inputs[i]->error(); return false;
                }
            }
            bitop_finish(operation, &out[0], mask, count);
            return true;
        };
        if (!target.replace_chunks(size, produce)) {
            if (read_status_ == read_status::missing) stored(target);
            return false;
        }
        if (!target.finish()) return stored(target);
        length = static_cast<long long>(size);
        return true;
    });
    if (result(status)) return true;
    length = 0; return false;
}

} // namespace dao
} // namespace pkv
