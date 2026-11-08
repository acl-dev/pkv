#include "stdafx.h"
#include "redis_set.h"
#include "redis_handler.h"
#include "dao/paged_set.h"
#include "common/redis_integer.h"
#include "coder/compact_set.h"

namespace pkv {

static void append_set_member(std::string& frame, uint64_t count,
    compact_set::member_view member) {
    if (frame.empty()) {
        frame += '*';
        frame += std::to_string(count);
        frame += "\r\n";
    }
    frame += '$';
    frame += std::to_string(member.size());
    frame += "\r\n";
    frame.append(member.data(), member.size());
    frame += "\r\n";
}

bool redis_set::exec(const char* cmd, redis_coder& result) {
    if (!strcasecmp(cmd, "SPOP")) return sample(true, result);
    if (!strcasecmp(cmd, "SRANDMEMBER")) return sample(false, result);
    if (!strcasecmp(cmd, "SINTERCARD")) return intersection_size(result);
    const auto is = [&](const char* name) { return !strcasecmp(cmd, name); };
    const bool add = is("SADD");
    if (add || is("SREM")) {
        if (check_cluster_mode(true) && route_keys(cmd, result)) return true;
        dao::paged_set data(handler_.page_pipeline_scope_);
        long long changed;
        auto& reply = result.create_object();
        auto& db = handler_.get_db();
        const auto& key = *obj_.argument(1);
        const bool ok = obj_.size() == 3 ?
            data.change_one(db, key, *obj_.argument(2), add, changed) :
            data.change_members(db, key,
                [&](size_t i) -> const std::string& { return *obj_.argument(i + 2); },
                obj_.size() - 2, add, changed);
        if (ok) reply.set_number(changed);
        else reply.set_error(data.error());
        return true;
    }
    const bool store = is("SUNIONSTORE") || is("SINTERSTORE") || is("SDIFFSTORE");
    const bool algebra = store || is("SUNION") || is("SINTER") || is("SDIFF");
    std::vector<std::string> keys;
    if (algebra) for (size_t i = 1; i < obj_.size(); ++i) keys.push_back(*obj_.argument(i));
    else {
        keys.push_back(*obj_.argument(1));
        if (is("SMOVE")) keys.push_back(*obj_.argument(2));
    }
    if (check_cluster_mode(true) && route_keys(keys, result)) return true;
    dao::paged_set data(handler_.page_pipeline_scope_);
    auto& db = handler_.get_db();
    auto& reply = result.create_object();
    const auto fail = [&]() { reply.set_error(data.error()); return true; };
    const auto& key = keys.front();
    if (is("SCARD")) {
        long long count;
        if (!data.size(db, key, count)) return fail();
        reply.set_number(count);
    } else if (is("SISMEMBER") || is("SMISMEMBER")) {
        const auto count = obj_.size() - 2;
        constexpr auto limit = db::page_lock_scope::max_membership_batch;
        if (count <= limit) {
            const std::string* members[limit];
            for (size_t i = 0; i < count; ++i) members[i] = obj_.argument(i + 2);
            unsigned found;
            if (!data.membership(db, key, members, count, found)) return fail();
            if (is("SISMEMBER")) reply.set_number(found);
            else {
                reply.set_type(REDIS_OBJ_ARRAY);
                for (size_t i = 0; i < count; ++i)
                    reply.create_child().set_number((found >> i) & 1U);
            }
            return true;
        }
        std::vector<std::string> values;
        for (size_t i = 2; i < obj_.size(); ++i) values.push_back(*obj_.argument(i));
        std::vector<long long> membership;
        if (!data.membership(db, key, values, membership)) return fail();
        reply.set_type(REDIS_OBJ_ARRAY);
        for (auto member : membership) reply.create_child().set_number(member);
    } else if (is("SMOVE")) {
        bool moved;
        if (!data.move(db, key, keys[1], *obj_.argument(3), moved)) return fail();
        reply.set_number(moved ? 1 : 0);
    } else if (is("SMEMBERS")) {
        std::vector<std::string> members;
        std::string frame;
        const auto append = [&](uint64_t count, const std::string& member) {
            if (count < 32) {
                members.push_back(member);
                return;
            }
            append_set_member(frame, count, member);
        };
        const auto append_packed = [&](const compact_set::view& packed) {
            // Each member adds at most seven bytes beyond its encoded payload.
            frame.reserve(5 + packed.payload_size() + 7 * packed.count());
            for (unsigned i = 0; i < packed.count(); ++i)
                append_set_member(frame, packed.count(), packed.at(i));
        };
        if (!data.visit_members(db, key, std::cref(append),
            std::cref(append_packed))) return fail();
        if (!frame.empty())
            reply.set_raw(std::move(frame)).response_as(redis_object::response_kind::raw_set);
        else {
            std::sort(members.begin(), members.end());
            reply.set_type(REDIS_OBJ_ARRAY);
            reply.response_as(redis_object::response_kind::set);
            for (const auto& member : members) reply.create_child().set_string(member);
        }
    } else {
        std::set<std::string> members;
        if (algebra) {
            const std::string* destination = store ? &keys.front() : nullptr;
            std::vector<std::string> sources(keys.begin() + (store ? 1 : 0), keys.end());
            const auto operation = is("SUNION") || is("SUNIONSTORE") ? dao::set_operation::unite
                : is("SINTER") || is("SINTERSTORE") ? dao::set_operation::intersect : dao::set_operation::difference;
            if (!data.algebra(db, sources, operation, destination, members)) return fail();
        } else return false;
        if (store) reply.set_number(static_cast<long long>(members.size()));
        else if (!members.empty()) {
            std::string frame;
            if (members.size() < 32) {
                size_t bytes = (members.size() < 10 ? 4 : 5) + 6 * members.size();
                for (const auto& member : members) {
                    bytes += member.size();
                    for (auto length = member.size(); length >= 10; length /= 10)
                        ++bytes;
                }
                frame.reserve(bytes);
            }
            for (const auto& member : members)
                append_set_member(frame, members.size(), member);
            reply.set_raw(std::move(frame)).response_as(redis_object::response_kind::raw_set);
        } else {
            reply.set_type(REDIS_OBJ_ARRAY);
            reply.response_as(redis_object::response_kind::set);
        }
    }
    return true;
}

bool redis_set::sample(bool remove, redis_coder& result) {
    if (obj_.size() > 3) { result.create_object().set_error("ERR syntax error"); return true; }
    const bool has_count = obj_.size() == 3;
    long long count = 1;
    if (has_count) {
        if (!parse_redis_integer(*obj_.argument(2), count)) {
            result.create_object().set_error(remove ? "ERR value is out of range, must be positive" : "ERR value is not an integer or out of range");
            return true;
        }
        if (remove && count < 0) { result.create_object().set_error("ERR value is out of range, must be positive"); return true; }
        if (count == LLONG_MIN) {
            result.create_object().set_error("ERR value is out of range, value must between -9223372036854775807 and 9223372036854775807");
            return true;
        }
    }
    if (check_cluster_mode(true) && route_keys(remove ? "SPOP" : "SRANDMEMBER", result)) return true;
    dao::paged_set data(handler_.page_pipeline_scope_);
    dao::set_sample_result out;
    auto& reply = result.create_object();
    if (!data.sample(handler_.get_db(), *obj_.argument(1), count, remove, out)) reply.set_error(data.error());
    else if (has_count && out.values.size() >= 32)
        reply.set_bulk_collection(out.values, remove);
    else if (has_count) {
        reply.set_type(REDIS_OBJ_ARRAY);
        if (remove) reply.response_as(redis_object::response_kind::set);
        for (const auto& member : out.values) reply.create_child().set_string(member);
    } else if (!out.found) reply.set_null();
    else reply.set_string(out.values.at(0));
    return true;
}

bool redis_set::intersection_size(redis_coder& result) {
    long long numkeys;
    if (!parse_redis_integer(*obj_.argument(1), numkeys) || numkeys <= 0) {
        result.create_object().set_error("ERR numkeys should be greater than 0"); return true;
    }
    if (static_cast<unsigned long long>(numkeys) > obj_.size() - 2) {
        result.create_object().set_error("ERR Number of keys can't be greater than number of args"); return true;
    }
    const auto end = static_cast<size_t>(numkeys) + 2;
    long long limit = 0;
    for (size_t i = end; i < obj_.size(); ++i) {
        const auto& option = *obj_.argument(i);
        if (option.size() != 5 || strncasecmp(option.data(), "LIMIT", 5) || i + 1 == obj_.size()) {
            result.create_object().set_error("ERR syntax error"); return true;
        }
        if (!parse_redis_integer(*obj_.argument(++i), limit) || limit < 0) {
            result.create_object().set_error("ERR LIMIT can't be negative"); return true;
        }
    }
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(numkeys));
    for (size_t i = 2; i < end; ++i) keys.push_back(*obj_.argument(i));
    if (check_cluster_mode(true) && route_keys(keys, result)) return true;
    dao::paged_set data;
    long long count;
    auto& reply = result.create_object();
    if (!data.intersection_size(handler_.get_db(), keys, limit, count)) reply.set_error(data.error());
    else reply.set_number(count);
    return true;
}

} // namespace pkv
