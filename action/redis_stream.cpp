#include "stdafx.h"
#include "redis_stream.h"
#include "redis_handler.h"
#include "db/stream_store.h"
#include "coder/record_codec.h"
#include "common/redis_integer.h"

namespace pkv {
namespace {
bool unsigned_id(const std::string& text, size_t first, size_t last, uint64_t& out) {
    if (first == last) return false;
    out = 0;
    for (size_t i = first; i < last; ++i) {
        const unsigned digit = static_cast<unsigned char>(text[i]) - '0';
        if (digit > 9 || out > (UINT64_MAX - digit) / 10) return false;
        out = out * 10 + digit;
    }
    return true;
}

bool parse_id(const std::string& text, stream_id& id, bool& sequence_auto) {
    const auto dash = text.find('-');
    if (dash == std::string::npos)
        return unsigned_id(text, 0, text.size(), id.milliseconds);
    if (!unsigned_id(text, 0, dash, id.milliseconds)) return false;
    sequence_auto = text.size() - dash == 2 && text.back() == '*';
    return sequence_auto || unsigned_id(text, dash + 1, text.size(), id.sequence);
}

const char* interval_id(const std::string& text, bool lower, stream_id& id) {
    if (text == "-") return nullptr;
    if (text == "+") {
        id.milliseconds = id.sequence = UINT64_MAX;
        return nullptr;
    }
    const bool exclusive = !text.empty() && text.front() == '(';
    const auto token = exclusive ? text.substr(1) : text;
    if (!lower && token.find('-') == std::string::npos) id.sequence = UINT64_MAX;
    bool automatic = false;
    if (!parse_id(token, id, automatic) || automatic)
        return "ERR Invalid stream ID specified as stream command argument";
    if (!exclusive) return nullptr;
    if (lower) {
        if (id.sequence != UINT64_MAX) ++id.sequence;
        else if (id.milliseconds != UINT64_MAX) { ++id.milliseconds; id.sequence = 0; }
        else return "ERR invalid start ID for the interval";
    } else {
        if (id.sequence) --id.sequence;
        else if (id.milliseconds) { --id.milliseconds; id.sequence = UINT64_MAX; }
        else return "ERR invalid end ID for the interval";
    }
    return nullptr;
}

std::string format_id(stream_id id) {
    return std::to_string(id.milliseconds) + "-" + std::to_string(id.sequence);
}

void reply_count(std::string& out, char prefix, size_t count) {
    out += prefix;
    out += std::to_string(count);
    out += "\r\n";
}

void reply_bulk(std::string& out, const std::string& bytes) {
    reply_count(out, '$', bytes.size());
    out += bytes;
    out += "\r\n";
}

bool next_id(stream_id& id, stream_id last, bool automatic, bool sequence_auto) {
    if (automatic) {
        const auto now = static_cast<uint64_t>(unix_time_ms());
        id.milliseconds = std::max(now, last.milliseconds);
        sequence_auto = true;
    }
    if (sequence_auto && id.milliseconds == last.milliseconds) {
        if (last.sequence != UINT64_MAX) id.sequence = last.sequence + 1;
        else if (automatic && last.milliseconds != UINT64_MAX) ++id.milliseconds;
        else return false;
    }
    return true;
}
} // namespace

bool redis_stream::exec(const char* name, redis_coder& result) {
    if (route_keys(name, result)) return true;
    auto& reply = result.create_object();
    const bool append = strcasecmp(name, "XADD") == 0;
    const bool reverse = strcasecmp(name, "XREVRANGE") == 0;
    const bool range = reverse || strcasecmp(name, "XRANGE") == 0;
    const auto& key = *obj_.argument(1);
    stream_id minimum, maximum;
    uint64_t limit = UINT64_MAX;
    if (range) {
        const auto* error = interval_id(*obj_.argument(reverse ? 3 : 2), true, minimum);
        if (!error) error = interval_id(*obj_.argument(reverse ? 2 : 3), false, maximum);
        if (error) { reply.set_error(error); return true; }
        for (size_t i = 4; i < obj_.size(); i += 2) {
            const auto& option = *obj_.argument(i);
            if (i + 1 == obj_.size() || option.size() != 5 ||
                strncasecmp(option.data(), "COUNT", 5)) {
                reply.set_error("ERR syntax error");
                return true;
            }
            long long count;
            if (!parse_redis_integer(*obj_.argument(i + 1), count)) {
                reply.set_error("ERR value is not an integer or out of range");
                return true;
            }
            limit = count < 0 ? 0 : static_cast<uint64_t>(count);
        }
    }
    stream_id entry_id;
    const std::string* inline_fields[8];
    std::vector<const std::string*> extra_fields;
    const std::string** fields = inline_fields;
    size_t field_count = 0;
    bool automatic = false, sequence_auto = false, no_create = false;
    if (append) {
        size_t id_index = 2;
        const auto& option = *obj_.argument(id_index);
        no_create = option.size() == 10 && !strncasecmp(option.data(), "NOMKSTREAM", 10);
        if (no_create) ++id_index;
        if (obj_.size() < id_index + 3 || (obj_.size() - id_index) % 2 != 1) {
            reply.set_error("ERR syntax error");
            return true;
        }
        const auto& id = *obj_.argument(id_index);
        automatic = id == "*";
        if (!automatic && !parse_id(id, entry_id, sequence_auto)) {
            reply.set_error("ERR Invalid stream ID specified as stream command argument");
            return true;
        }
        if (!automatic && !sequence_auto && !entry_id.milliseconds && !entry_id.sequence) {
            reply.set_error("ERR The ID specified in XADD must be greater than 0-0");
            return true;
        }
        field_count = obj_.size() - id_index - 1;
        if (field_count > sizeof(inline_fields) / sizeof(inline_fields[0])) {
            extra_fields.resize(field_count);
            fields = extra_fields.data();
        }
        for (size_t i = 0; i < field_count; ++i)
            fields[i] = obj_.argument(id_index + 1 + i);
    }
    auto& db = handler_.get_db();
    const char* error = "ERR stream storage error";
    uint64_t size = 0;
    bool skipped = false;
    std::string generated_id;
    std::vector<stream_entry> entries;
    const auto apply = [&](page_transaction& tx) {
        auto& root = *tx.roots().find(key);
        if (root.found && root.value.compare(0, 3, "PXR") != 0) {
            record_header header;
            if (db->value_codec().stored_header(root.value, header) != codec_status::ok)
                return false;
            if (header.expires_at_ms >= 0 && header.expires_at_ms <= unix_time_ms())
                root.action = db::update_action::erase;
            else if (header.kind != value_kind::stream) {
                error = "WRONGTYPE Operation against a key holding the wrong kind of value";
                return false;
            }
        }
        stream_store stream(tx, key);
        if (stream.open(true) != stream_store::status::ok) return false;
        size = stream.size();
        if (range) return stream.range(minimum, maximum, reverse, limit, entries) ==
            stream_store::status::ok;
        if (!append) return true;
        if (no_create && !size) { skipped = true; return true; }
        if (!next_id(entry_id, stream.last_id(), automatic, sequence_auto)) {
            error = "ERR The stream has exhausted the last possible ID";
            return false;
        }
        const auto written = stream.append(entry_id, fields, field_count);
        if (written == stream_store::status::invalid_id)
            error = "ERR The ID specified in XADD is equal or smaller than the target stream top item";
        else if (written == stream_store::status::too_large)
            error = "ERR stream entry exceeds storage limits";
        if (written != stream_store::status::ok) return false;
        generated_id = format_id(entry_id);
        return true;
    };
    auto* scope = handler_.page_pipeline_scope_;
    const auto status = scope ? scope->transact(*db, key, std::ref(apply)) :
        db->transact_page(key, std::ref(apply));
    if (status != db::batch_status::ok) reply.set_error(error);
    else if (range) {
        if (!limit && size) {
            reply.set_type(REDIS_OBJ_NULL_ARRAY);
        } else {
            // The nested arrays and bulk strings have the same wire form
            // in RESP2 and RESP3.
            std::string frame;
            reply_count(frame, '*', entries.size());
            for (const auto& value : entries) {
                frame += "*2\r\n";
                reply_bulk(frame, format_id(value.id));
                reply_count(frame, '*', value.fields.size() * 2);
                for (const auto& field : value.fields) {
                    reply_bulk(frame, field.first);
                    reply_bulk(frame, field.second);
                }
            }
            reply.set_raw(std::move(frame));
        }
    } else if (!append) reply.set_number(static_cast<long long>(size));
    else if (skipped) reply.set_null();
    else reply.set_string(generated_id);
    return true;
}

} // namespace pkv
