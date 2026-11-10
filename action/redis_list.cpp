#include "stdafx.h"
#include "redis_list.h"
#include "redis_handler.h"
#include "dao/paged_list.h"
#include "common/redis_integer.h"
#include "common/list_values_workspace.h"
#include <cstring>

namespace pkv {

// Shared by direction, insertion and search option parsing.
static bool list_keyword(const std::string& value, const char* keyword);

redis_list::operation redis_list::classify(const char* name) {
    struct binding { const char* name; operation op; };
    static const binding entries[] = {
        {"BLPOP", operation::blpop},
        {"BRPOP", operation::brpop},
        {"BLMPOP", operation::blmpop},
        {"BLMOVE", operation::blmove},
        {"BRPOPLPUSH", operation::brpoplpush},
        {"LMOVE", operation::lmove},
        {"RPOPLPUSH", operation::rpoplpush},
        {"LMPOP", operation::lmpop},
        {"LPOS", operation::lpos},
        {"LPUSH", operation::lpush},
        {"RPUSH", operation::rpush},
        {"LPUSHX", operation::lpushx},
        {"RPUSHX", operation::rpushx},
        {"LPOP", operation::lpop},
        {"RPOP", operation::rpop},
        {"LINSERT", operation::linsert},
        {"LREM", operation::lrem},
        {"LLEN", operation::llen},
        {"LINDEX", operation::lindex},
        {"LSET", operation::lset},
        {"LRANGE", operation::lrange},
        {"LTRIM", operation::ltrim},
    };
    for (const auto& entry : entries)
        if (!strcasecmp(name, entry.name)) return entry.op;
    return operation::unknown;
}

redis_handler_t redis_list::bind(const char* name) {
    const auto op = classify(name);
    if (op == operation::unknown) return {};
    return [op](redis_handler& handler, const char* cmd,
        const redis_object& object, redis_coder& result) {
        redis_list command(handler, object);
        return command.execute(op, cmd, result);
    };
}

bool redis_list::exec(const char* cmd, redis_coder& result) {
    return execute(classify(cmd), cmd, result);
}

bool redis_list::execute(operation op, const char* cmd, redis_coder& result) {
    if (op == operation::blpop || op == operation::brpop ||
        op == operation::blmpop || op == operation::blmove ||
        op == operation::brpoplpush) return blocking(cmd, result);
    if (op == operation::lmove) return move(false, result);
    if (op == operation::rpoplpush) return move(true, result);
    if (op == operation::lmpop) return mpop(result);
    if (op == operation::lpos) return positions(result);
    if (check_cluster_mode(true) && route_keys(cmd, result)) return true;
    dao::paged_list data(handler_.page_pipeline_scope_);
    auto& reply = result.create_object();
    auto& db = handler_.get_db();
    const auto& key = *obj_.argument(1);
    const auto is = [op](operation selected) { return op == selected; };
    const auto fail = [&]() { reply.set_error(data.error()); return true; };
    const auto integer_error = [&]() {
        reply.set_error("ERR value is not an integer or out of range");
        return true;
    };
    if (is(operation::lpush) || is(operation::rpush) ||
        is(operation::lpushx) || is(operation::rpushx)) {
        long long length;
        const bool left = is(operation::lpush) || is(operation::lpushx);
        const bool only_existing = is(operation::lpushx) ||
            is(operation::rpushx);
        if (obj_.size() == 3) {
            if (!data.push(db, key, list_values_view(obj_.argument(2), 1),
                left, only_existing, length)) return fail();
        } else {
            detail::list_values_workspace workspace;
            auto& values = workspace.values();
            values.resize(obj_.size() - 2);
            for (size_t i = 2; i < obj_.size(); ++i)
                values[i - 2] = *obj_.argument(i);
            if (!data.push(db, key, values, left, only_existing, length))
                return fail();
        }
        reply.set_number(length);
    } else if (is(operation::lpop) || is(operation::rpop)) {
        const bool left = is(operation::lpop);
        const bool counted = obj_.size() == 3;
        long long count = 1;
        if (counted && (!parse_redis_integer(*obj_.argument(2), count) || count < 0)) {
            reply.set_error("ERR value is out of range, must be positive");
            return true;
        }
        if (count == 1) {
            std::string value;
            const bool direct = !counted && handler_.page_pipeline_scope_;
            auto& payload = direct ? reply.string_buffer() : value;
            bool found;
            if (!data.pop_one(db, key, left, payload, found)) return fail();
            if (!found)
                reply.set_type(counted ? REDIS_OBJ_NULL_ARRAY : REDIS_OBJ_NULL_STRING);
            else if (counted) reply.set_bulk_collection(&value, 1);
            else if (!direct) reply.set_string(value);
            return true;
        }
        dao::list_result out;
        if (!data.pop(db, key, left, count, out)) return fail();
        if (!out.found) reply.set_type(REDIS_OBJ_NULL_ARRAY);
        else reply.set_bulk_collection(out.values);
    } else if (is(operation::linsert)) {
        const auto& option = *obj_.argument(2);
        const bool before = list_keyword(option, "BEFORE");
        if (!before && !list_keyword(option, "AFTER")) {
            reply.set_error("ERR syntax error"); return true;
        }
        long long length;
        if (!data.insert(db, key, before, *obj_.argument(3), *obj_.argument(4),
            length)) return fail();
        reply.set_number(length);
    } else if (is(operation::lrem)) {
        long long count, removed;
        if (!parse_redis_integer(*obj_.argument(2), count))
            return integer_error();
        if (!data.remove(db, key, count, *obj_.argument(3), removed))
            return fail();
        reply.set_number(removed);
    } else if (is(operation::llen)) {
        long long length;
        if (!data.length(db, key, length)) return fail();
        reply.set_number(length);
    } else if (is(operation::lindex)) {
        dao::list_result out;
        if (!data.index(db, key, *obj_.argument(2), out)) return fail();
        if (!out.found) reply.set_null();
        else reply.set_string(out.values.at(0));
    } else if (is(operation::lset)) {
        if (!data.set(db, key, *obj_.argument(2), *obj_.argument(3)))
            return fail();
        reply.set_status("OK");
    } else if (is(operation::lrange) || is(operation::ltrim)) {
        long long start, end;
        if (!parse_redis_integer(*obj_.argument(2), start) ||
            !parse_redis_integer(*obj_.argument(3), end))
            return integer_error();
        if (is(operation::ltrim)) {
            if (!data.trim(db, key, start, end)) return fail();
            reply.set_status("OK");
        } else {
            std::vector<std::string> values;
            if (!data.range(db, key, start, end, values)) return fail();
            // RESP2 and RESP3 use the same array of bulk strings here.
            reply.set_bulk_collection(values);
        }
    } else return false;
    return true;
}

// Keywords are byte strings: a NUL suffix must not turn an unknown option into
// a direction or COUNT token. Match complete argument lengths.
static bool list_keyword(const std::string& value, const char* keyword) {
    const size_t length = std::strlen(keyword);
    return value.size() == length &&
        !strncasecmp(value.data(), keyword, length);
}

bool redis_list::move(bool legacy, redis_coder& result) {
    bool from_left = false, to_left = true;
    if (!legacy) {
        const auto& from = *obj_.argument(3);
        const auto& to = *obj_.argument(4);
        if ((!list_keyword(from, "LEFT") && !list_keyword(from, "RIGHT")) ||
            (!list_keyword(to, "LEFT") && !list_keyword(to, "RIGHT"))) {
            result.create_object().set_error("ERR syntax error");
            return true;
        }
        from_left = list_keyword(from, "LEFT");
        to_left = list_keyword(to, "LEFT");
    }
    const auto& source = *obj_.argument(1);
    const auto& destination = *obj_.argument(2);
    if (check_cluster_mode(true) && route_keys(
        std::vector<std::string>{source, destination}, result)) return true;
    dao::paged_list data;
    dao::list_result out;
    auto& reply = result.create_object();
    if (!data.move(handler_.get_db(), source, destination, from_left, to_left,
        out)) reply.set_error(data.error());
    else if (!out.found) reply.set_null();
    else reply.set_string(out.values.at(0));
    return true;
}

bool redis_list::mpop(redis_coder& result) {
    long long numkeys;
    if (!parse_redis_integer(*obj_.argument(1), numkeys) || numkeys <= 0) {
        result.create_object().set_error(
            "ERR numkeys should be greater than 0");
        return true;
    }
    // Check against the actual frame size before addition or conversion to an
    // unsigned index, including LLONG_MAX supplied as numkeys.
    if (static_cast<unsigned long long>(numkeys) > obj_.size() - 3) {
        result.create_object().set_error("ERR syntax error");
        return true;
    }
    const size_t where = static_cast<size_t>(numkeys) + 2;
    const auto& direction = *obj_.argument(where);
    const bool left = list_keyword(direction, "LEFT");
    if (!left && !list_keyword(direction, "RIGHT")) {
        result.create_object().set_error("ERR syntax error");
        return true;
    }
    long long count = 1;
    bool has_count = false;
    for (size_t i = where + 1; i < obj_.size(); ++i) {
        if (has_count || !list_keyword(*obj_.argument(i), "COUNT") ||
            i + 1 == obj_.size()) {
            result.create_object().set_error("ERR syntax error");
            return true;
        }
        has_count = true;
        if (!parse_redis_integer(*obj_.argument(++i), count) || count <= 0) {
            result.create_object().set_error(
                "ERR count should be greater than 0");
            return true;
        }
    }
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(numkeys));
    for (size_t i = 2; i < where; ++i) keys.push_back(*obj_.argument(i));
    if (route_keys(keys, result)) return true;
    dao::paged_list data;
    dao::list_result out;
    std::string selected_key;
    auto& reply = result.create_object();
    if (!data.mpop(handler_.get_db(), keys, left, count, selected_key, out))
        reply.set_error(data.error());
    else if (!out.found) reply.set_type(REDIS_OBJ_NULL_ARRAY);
    else {
        reply.set_type(REDIS_OBJ_ARRAY);
        reply.create_child().set_string(selected_key);
        auto& items = reply.create_child();
        items.set_type(REDIS_OBJ_ARRAY);
        for (const auto& value : out.values)
            items.create_child().set_string(value);
    }
    return true;
}

bool redis_list::positions(redis_coder& result) {
    if (check_cluster_mode(true) && route_keys("LPOS", result)) return true;
    list_search options;
    bool has_count = false;
    for (size_t i = 3; i < obj_.size(); ++i) {
        const auto& option = *obj_.argument(i);
        if (i + 1 == obj_.size()) {
            result.create_object().set_error("ERR syntax error"); return true;
        }
        const auto& amount = *obj_.argument(++i);
        if (list_keyword(option, "RANK")) {
            if (!parse_redis_integer(amount, options.rank)) {
                result.create_object().set_error(
                    "ERR value is not an integer or out of range");
                return true;
            }
            if (options.rank == LLONG_MIN) {
                result.create_object().set_error("ERR value is out of range, "
                    "value must between -9223372036854775807 and "
                    "9223372036854775807");
                return true;
            }
            if (!options.rank) {
                result.create_object().set_error("ERR RANK can't be zero: "
                    "use 1 to start from the first match, 2 from the second "
                    "... or use negative to start from the end of the list");
                return true;
            }
        } else if (list_keyword(option, "COUNT")) {
            has_count = true;
            if (!parse_redis_integer(amount, options.count) ||
                options.count < 0) {
                result.create_object().set_error("ERR COUNT can't be negative");
                return true;
            }
        } else if (list_keyword(option, "MAXLEN")) {
            if (!parse_redis_integer(amount, options.maxlen) ||
                options.maxlen < 0) {
                result.create_object().set_error(
                    "ERR MAXLEN can't be negative");
                return true;
            }
        } else {
            result.create_object().set_error("ERR syntax error"); return true;
        }
    }
    dao::paged_list data;
    std::vector<long long> indexes;
    auto& reply = result.create_object();
    if (!data.positions(handler_.get_db(), *obj_.argument(1), *obj_.argument(2),
        options, indexes)) reply.set_error(data.error());
    else if (has_count) {
        reply.set_type(REDIS_OBJ_ARRAY);
        for (auto index : indexes) reply.create_child().set_number(index);
    } else if (indexes.empty()) reply.set_null();
    else reply.set_number(indexes.front());
    return true;
}

} // namespace pkv
