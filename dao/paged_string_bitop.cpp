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
        std::map<std::string, std::unique_ptr<string_bitop_source>> distinct;
        std::vector<string_bitop_source*> inputs;
        uint64_t size = 0;
        for (const auto& key : sources) {
            auto found = distinct.find(key);
            if (found == distinct.end()) {
                std::unique_ptr<string_bitop_source> value(
                    new string_bitop_source(db, tx, key, now));
                if (!value->open()) {
                    read_status_ = value->error(); return false;
                }
                size = std::max(size, value->length());
                found = distinct.emplace(key, std::move(value)).first;
            }
            inputs.push_back(found->second.get());
        }
        if (!size) {
            auto& entry = *tx.roots().find(destination);
            if (entry.found) entry.action = db::update_action::erase;
            return true;
        }
        string_store target(tx, destination);
        const auto produce = [&](uint64_t offset, size_t count,
            std::string& out) {
            std::string state(count, '\0'), input;
            for (size_t i = 0; i < inputs.size(); ++i) {
                auto& bytes = i == 0 ? out : input;
                if (!inputs[i]->read(offset, count, bytes)) {
                    read_status_ = inputs[i]->error(); return false;
                }
                bytes.resize(count, '\0');
                if (i) bitop_fold(operation, &out[0], &state[0],
                    bytes.data(), count);
            }
            bitop_finish(operation, &out[0], &state[0], count);
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
