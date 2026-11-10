#include "db/mdb/mdb.h"
#include "db/stream_store.h"
#include <cstdlib>
#include <iostream>

using namespace pkv;

static void require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

static stream_store::status append(stream_store& stream, const stream_entry& entry) {
    std::vector<const std::string*> fields;
    for (const auto& field : entry.fields) {
        fields.push_back(&field.first);
        fields.push_back(&field.second);
    }
    return stream.append(entry.id, fields.data(), fields.size());
}

int main() {
    using status = stream_store::status;
    mdb database(1);
    require(database.open(""), "open MDB");
    const std::string key = "stream";
    stream_entry first;
    first.id.sequence = 1;
    first.fields.emplace_back("name", std::string("x\0\r\ny", 5));
    first.fields.emplace_back("name", "");
    auto second = first;
    second.id.milliseconds = 1;
    second.id.sequence = 0;

    require(database.transact_page(key, [&](page_transaction& tx) {
        stream_store stream(tx, key);
        stream_entry read;
        return stream.open() == status::ok && stream.size() == 0 &&
            append(stream, first) == status::ok &&
            append(stream, first) == status::invalid_id &&
            append(stream, second) == status::ok &&
            stream.read(1, read) == status::ok && read.fields == first.fields;
    }) == db::batch_status::ok, "append and read staged entries");

    auto third = second;
    third.id.sequence = 1;
    require(database.transact_page(key, [&](page_transaction& tx) {
        stream_store stream(tx, key);
        require(stream.open() == status::ok &&
            append(stream, third) == status::ok, "stage rollback entry");
        return false;
    }) == db::batch_status::aborted, "reject append");
    require(database.transact_page(key, [&](page_transaction& tx) {
        stream_store stream(tx, key);
        stream_entry read;
        const std::string chunk(page_transaction::max_value_bytes, 'x');
        const std::vector<const std::string*> oversized(2050, &chunk);
        return stream.open() == status::ok && stream.size() == 2 &&
            stream.last_id().milliseconds == 1 && stream.last_id().sequence == 0 &&
            stream.append(third.id, oversized.data(), oversized.size()) ==
                status::too_large &&
            stream.read(2, read) == status::ok && read.fields == second.fields &&
            read.id.milliseconds == 1 && read.id.sequence == 0 &&
            stream.read(3, read) == status::invalid && read.fields == second.fields;
    }) == db::batch_status::ok, "verify rollback, size limit and unchanged read output");
    third.fields.front().second.assign(50000, '\0');
    third.fields.front().second.back() = 'z';
    require(database.transact_page(key, [&](page_transaction& tx) {
        stream_store stream(tx, key);
        stream_entry read;
        return stream.open() == status::ok &&
            append(stream, third) == status::ok &&
            stream.read(3, read) == status::ok && read.fields == third.fields;
    }) == db::batch_status::ok, "append fragmented entry");
    require(database.transact_page(key, [&](page_transaction& tx) {
        stream_store stream(tx, key);
        std::vector<stream_entry> result;
        return stream.open() == status::ok && stream.size() == 3 &&
            stream.range(second.id, third.id, true, 2, result) == status::ok &&
            result.size() == 2 && result.front().fields == third.fields &&
            result.back().fields == second.fields;
    }) == db::batch_status::ok, "read fragmented entry by ID");
    stream_entry compact;
    compact.id.milliseconds = (uint64_t(1) << 48) - 1;
    compact.id.sequence = UINT16_MAX;
    compact.fields.emplace_back("myfield", std::string("a\0bcd", 5));
    require(database.transact_page(key, [&](page_transaction& tx) {
        stream_store stream(tx, key);
        std::string bytes;
        std::vector<stream_entry> result;
        return stream.open() == status::ok && append(stream, compact) == status::ok &&
            tx.roots().find(key)->value[3] == 3 &&
            tx.roots().find(key)->value.size() == 16 &&
            tx.lookup(key, 4, bytes) == db::lookup_status::found && bytes.size() == 22 &&
            stream.range(third.id, compact.id, false, 2, result) == status::ok &&
            result.size() == 2 && result.front().fields == third.fields &&
            result.back().fields == compact.fields &&
            result.back().id.milliseconds == compact.id.milliseconds &&
            result.back().id.sequence == compact.id.sequence;
    }) == db::batch_status::ok, "read compact entry in a mixed stream");
    stream_root root, decoded;
    root.compact_entries = true;
    root.size = (uint64_t(1) << 31) - 1;
    root.last = compact.id;
    root.expires_at_ms = static_cast<int64_t>(compact.id.milliseconds);
    std::string encoded;
    encode_stream_root(root, encoded);
    require(encoded.size() == 22 && encoded[3] == 3 &&
        decode_stream_root(encoded, decoded) && decoded.size == root.size &&
        decoded.last.milliseconds == root.last.milliseconds &&
        decoded.last.sequence == root.last.sequence &&
        decoded.expires_at_ms == root.expires_at_ms && decoded.compact_entries,
        "expiring compact root round trip");
    encoded.pop_back();
    require(!decode_stream_root(encoded, decoded), "reject truncated compact root");
    ++root.last.sequence;
    encode_stream_root(root, encoded);
    require(encoded.size() == 36 && encoded[3] == 2 &&
        decode_stream_root(encoded, decoded) && decoded.last.sequence == root.last.sequence,
        "wide sequence uses legacy root");
}
