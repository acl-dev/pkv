#include "stdafx.h"
#include "redis_zset.h"
#include "redis_blocking_pop.h"
#include "redis_handler.h"
#include "dao/paged_zset.h"
#include "common/redis_double.h"
#include "common/redis_integer.h"
#include <array>

namespace pkv {
namespace {
bool option_is(const std::string& value, const char* name) {
    const auto length = std::strlen(name);
    return value.size() == length && !strncasecmp(value.data(), name, length);
}
void emit_score(redis_object& reply, double value) {
    reply.set_string(format_redis_double(value));
    reply.response_as(redis_object::response_kind::real);
}
void append_bulk(std::string& frame, const std::string& bytes) {
    frame += '$';
    frame += std::to_string(bytes.size());
    frame += "\r\n";
    frame += bytes;
    frame += "\r\n";
}
void emit(redis_object& reply, const std::vector<zset_member>& values, bool
    scores) {
    reply.set_type(REDIS_OBJ_ARRAY);
    if (scores) reply.response_as(redis_object::response_kind::pairs);
    for (const auto& value : values) {
        reply.create_child().set_string(value.member);
        if (scores) emit_score(reply.create_child(), value.score);
    }
}
// Common scored ranges can size their RESP buffer exactly, avoiding
// geometric growth while many replies are queued on a pipelined connection.
template<size_t Capacity>
void emit_scored_sized(redis_object& reply,
    const std::vector<zset_member>& values, bool pairs) {
    std::array<std::string, Capacity> scores;
    std::string frame(1, '*');
    frame += std::to_string(pairs ? values.size() : values.size() * 2);
    frame += "\r\n";
    size_t size = frame.size();
    auto account = [&](size_t bytes, size_t overhead) {
        if (overhead > frame.max_size() - size ||
            bytes > frame.max_size() - size - overhead)
            throw std::length_error("sorted set reply too large");
        size += overhead + bytes;
    };
    auto bulk_overhead = [](size_t bytes) {
        size_t result = 6;
        for (; bytes >= 10; bytes /= 10) ++result;
        return result;
    };
    for (size_t i = 0; i < values.size(); ++i) {
        const auto length = values[i].member.size();
        account(length, bulk_overhead(length));
        scores[i] = format_redis_double(values[i].score);
        const auto score = scores[i].size();
        account(score, pairs ? 7 : bulk_overhead(score));
    }
    frame.reserve(size);
    for (size_t i = 0; i < values.size(); ++i) {
        if (pairs) frame += "*2\r\n";
        append_bulk(frame, values[i].member);
        if (pairs) {
            frame += ',';
            frame += scores[i];
            frame += "\r\n";
        } else append_bulk(frame, scores[i]);
    }
    reply.set_raw(std::move(frame));
}

void emit_scored(redis_object& reply, const std::vector<zset_member>& values,
    unsigned protocol) {
    if (values.size() < 32) {
        emit(reply, values, true);
        return;
    }
    const bool pairs = protocol == 3;
    if (values.size() == 32) {
        emit_scored_sized<32>(reply, values, pairs);
        return;
    }
    if (values.size() <= 128) {
        emit_scored_sized<128>(reply, values, pairs);
        return;
    }
    std::string frame;
    frame.reserve(values.size() * 24);
    frame += '*';
    frame += std::to_string(pairs ? values.size() : values.size() * 2);
    frame += "\r\n";
    for (const auto& value : values) {
        if (pairs) frame += "*2\r\n";
        append_bulk(frame, value.member);
        const auto score = format_redis_double(value.score);
        if (pairs) {
            frame += ',';
            frame += score;
            frame += "\r\n";
        } else append_bulk(frame, score);
    }
    reply.set_raw(std::move(frame));
}

}
bool redis_zset::exec(const char* cmd, redis_coder& result) {
    const auto is = [&](const char* name) { return !strcasecmp(cmd, name); };
    if (is("ZADD")) return add(false, result);
    if (is("ZINCRBY")) return add(true, result);
    if (is("BZPOPMIN") || is("BZPOPMAX") || is("BZMPOP")) {
        redis_blocking_pop operation(handler_, obj_);
        return operation.exec(cmd, result);
    }
    if (is("ZRANGE") || is("ZREVRANGE") || is("ZRANGESTORE") ||
        is("ZRANGEBYSCORE") || is("ZREVRANGEBYSCORE") ||
        is("ZRANGEBYLEX") || is("ZREVRANGEBYLEX") || is("ZCOUNT") ||
        is("ZLEXCOUNT") || is("ZREMRANGEBYRANK") ||
        is("ZREMRANGEBYSCORE") || is("ZREMRANGEBYLEX"))
        return select(cmd, result);
    if (is("ZUNION") || is("ZINTER") || is("ZDIFF") || is("ZUNIONSTORE") ||
        is("ZINTERSTORE") || is("ZDIFFSTORE") || is("ZINTERCARD"))
        return algebra(cmd, result);
    if (is("ZMPOP")) return mpop(result);
    if (is("ZRANDMEMBER")) return sample(result);
    if (check_cluster_mode(true) && route_keys(cmd, result)) return true;
    auto* scope = handler_.page_pipeline_scope_;
    if (scope) scope->cache_repeated_map_read(obj_.argument(2));
    dao::paged_zset data(scope);
    auto& db = handler_.get_db();
    const auto& key = *obj_.argument(1);
    auto& reply = result.create_object();
    const auto fail = [&]() { reply.set_error(data.error()); return true; };
    if (is("ZCARD")) {
        long long count;
        if (!data.size(db, key, count)) return fail();
        reply.set_number(count);
    } else if (is("ZREM")) {
        std::vector<std::string> names;
        for (size_t i = 2; i < obj_.size(); ++i)
            names.push_back(*obj_.argument(i));
        long long removed;
        if (!data.remove(db, key, names, removed)) return fail();
        reply.set_number(removed);
    } else if (is("ZSCORE")) {
        zset_score score;
        if (!data.score(db, key, *obj_.argument(2), score)) return fail();
        if (score.found) emit_score(reply, score.score);
        else reply.set_null();
    } else if (is("ZMSCORE")) {
        const auto count = obj_.size() - 2;
        const auto emit_scores = [&](const zset_score* scores, size_t size) {
            reply.set_type(REDIS_OBJ_ARRAY);
            for (size_t i = 0; i < size; ++i) {
                auto& child = reply.create_child();
                if (scores[i].found) emit_score(child, scores[i].score);
                else child.set_null();
            }
        };
        constexpr auto limit = db::page_lock_scope::max_score_batch;
        if (count <= limit) {
            const std::string* names[limit];
            zset_score scores[limit];
            for (size_t i = 0; i < count; ++i) names[i] = obj_.argument(i + 2);
            if (!data.scores(db, key, names, count, scores)) return fail();
            emit_scores(scores, count);
            return true;
        }
        std::vector<const std::string*> names;
        names.reserve(count);
        for (size_t i = 2; i < obj_.size(); ++i)
            names.push_back(obj_.argument(i));
        std::vector<zset_score> scores(count);
        if (!data.scores(db, key, names.data(), count, scores.data()))
            return fail();
        const bool resp3 = handler_.client().protocol() == 3;
        std::string frame = "*" + std::to_string(count) + "\r\n";
        for (const auto& score : scores) {
            if (!score.found) {
                frame += resp3 ? "_\r\n" : "$-1\r\n";
            } else {
                const auto text = format_redis_double(score.score);
                if (resp3) {
                    frame += ',';
                    frame += text;
                    frame += "\r\n";
                } else append_bulk(frame, text);
            }
        }
        reply.set_raw(std::move(frame));
    } else if (is("ZRANK") || is("ZREVRANK")) {
        const bool with_score = obj_.size() == 4;
        if (with_score && !option_is(*obj_.argument(3), "WITHSCORE")) {
            reply.set_error("ERR syntax error"); return true; }
        zset_rank rank;
        if (!data.rank(db, key, *obj_.argument(2), is("ZREVRANK"), rank))
            return fail();
        if (!rank.found) reply.set_type(with_score ? REDIS_OBJ_NULL_ARRAY :
            REDIS_OBJ_NULL_STRING);
        else if (!with_score) reply.set_number(rank.rank);
        else {
            reply.set_type(REDIS_OBJ_ARRAY);
            reply.create_child().set_number(rank.rank);
            emit_score(reply.create_child(), rank.score);
        }
    } else return false;
    return true;
}

bool redis_zset::pop(bool maximum, redis_coder& result) {
    if (check_cluster_mode(true) &&
        route_keys(maximum ? "ZPOPMAX" : "ZPOPMIN", result)) return true;
    auto& reply = result.create_object();
    if (obj_.size() > 3) {
        reply.set_error("ERR syntax error"); return true;
    }
    long long count = 1;
    if (obj_.size() == 3 && (!parse_redis_integer(*obj_.argument(2), count)
        || count < 0)) {
        reply.set_error("ERR value is out of range, must be positive");
        return true;
    }
    dao::paged_zset data(handler_.page_pipeline_scope_);
    if (count == 1) {
        zset_member value;
        bool found;
        if (!data.pop_one(handler_.get_db(), *obj_.argument(1), maximum,
            value, found)) {
            reply.set_error(data.error());
            return true;
        }
        reply.set_type(REDIS_OBJ_ARRAY);
        if (obj_.size() == 3)
            reply.response_as(redis_object::response_kind::pairs);
        if (found) {
            reply.create_child().set_string(value.member);
            emit_score(reply.create_child(), value.score);
        }
        return true;
    }
    std::vector<zset_member> values;
    if (!data.pop(handler_.get_db(), *obj_.argument(1), count, maximum, values)) {
        reply.set_error(data.error());
        return true;
    }
    emit_scored(reply, values, handler_.client().protocol());
    return true;
}

bool redis_zset::algebra(const char* cmd, redis_coder& result) {
    const bool store = !strcasecmp(cmd, "ZUNIONSTORE") || !strcasecmp(cmd,
        "ZINTERSTORE") || !strcasecmp(cmd, "ZDIFFSTORE");
    const bool card = !strcasecmp(cmd, "ZINTERCARD");
    const auto operation = !strncasecmp(cmd, "ZUNION", 6) ?
        dao::zset_operation::unite :
        !strncasecmp(cmd, "ZDIFF", 5) ? dao::zset_operation::difference :
            dao::zset_operation::intersect;
    const size_t number = store ? 2 : 1;
    const auto error = [&](const std::string& message) {
        result.create_object().set_error(message); return true; };
    long long numkeys;
    if (!parse_redis_integer(*obj_.argument(number), numkeys)) return error(
        "ERR value is not an integer or out of range");
    if (numkeys < 1) {
        std::string name(cmd);
        for (auto& ch : name) ch =
            static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return error("ERR at least 1 input key is needed for '" + name +
            "' command");
    }
    if (static_cast<unsigned long long>(numkeys) > obj_.size() - number - 1)
        return error("ERR syntax error");
    const auto end = number + 1 + static_cast<size_t>(numkeys);
    std::vector<std::string> sources;
    sources.reserve(static_cast<size_t>(numkeys));
    for (size_t i = number + 1; i < end; ++i)
        sources.push_back(*obj_.argument(i));
    if (check_cluster_mode(true)) {
        if (store) {
            auto locked = sources;
            locked.push_back(*obj_.argument(1));
            if (route_keys(locked, result)) return true;
        } else if (route_keys(sources, result)) return true;
    }
    bool scores = false;
    std::string parse_error;
    const auto parse = [&](dao::zset_algebra_options& options) {
        const auto fail = [&](const char* message) { parse_error = message;
            return false; };
        for (size_t i = end; i < obj_.size(); ++i) {
            const auto& option = *obj_.argument(i);
            if (!card && operation != dao::zset_operation::difference &&
                option_is(option, "WEIGHTS") && obj_.size() - i - 1 >=
                sources.size()) {
                for (size_t j = 0; j < sources.size(); ++j)
                    if (!parse_redis_double(*obj_.argument(++i),
                        options.weights[j])) return fail(
                        "ERR weight value is not a float");
            } else if (!card && operation != dao::zset_operation::difference &&
                option_is(option, "AGGREGATE") && i + 1 < obj_.size()) {
                const auto& aggregate = *obj_.argument(++i);
                if (option_is(aggregate, "SUM")) options.aggregate =
                    dao::zset_aggregate::sum;
                else if (option_is(aggregate, "MIN")) options.aggregate =
                    dao::zset_aggregate::minimum;
                else if (option_is(aggregate, "MAX")) options.aggregate =
                    dao::zset_aggregate::maximum;
                else return fail("ERR syntax error");
            } else if (!store && !card && option_is(option, "WITHSCORES"))
                scores = true;
            else if (card && option_is(option, "LIMIT") &&
                i + 1 < obj_.size()) {
                if (!parse_redis_integer(*obj_.argument(++i), options.limit) ||
                    options.limit < 0)
                    return fail("ERR LIMIT can't be negative");
            } else return fail("ERR syntax error");
        }
        return true;
    };
    dao::paged_zset data;
    std::vector<zset_member> values;
    long long cardinality;
    if (!data.algebra(handler_.get_db(), sources, operation, store ?
        obj_.argument(1) : nullptr, card, parse, values, cardinality))
        return error(parse_error.empty() ? data.error() : parse_error);
    auto& reply = result.create_object();
    if (store || card) reply.set_number(cardinality);
    else emit(reply, values, scores);
    return true;
}

bool redis_zset::mpop(redis_coder& result) {
    const auto error = [&](const char* message) {
        result.create_object().set_error(message); return true; };
    long long numkeys;
    if (!parse_redis_integer(*obj_.argument(1), numkeys) || numkeys <= 0)
        return error("ERR numkeys should be greater than 0");
    if (static_cast<unsigned long long>(numkeys) > obj_.size() - 3)
        return error("ERR syntax error");
    const size_t where = static_cast<size_t>(numkeys) + 2;
    const bool maximum = option_is(*obj_.argument(where), "MAX");
    if (!maximum && !option_is(*obj_.argument(where), "MIN")) return error(
        "ERR syntax error");
    long long count = 1;
    bool has_count = false;
    for (size_t i = where + 1; i < obj_.size(); ++i) {
        if (has_count || !option_is(*obj_.argument(i), "COUNT") || i + 1 ==
            obj_.size()) return error("ERR syntax error");
        has_count = true;
        if (!parse_redis_integer(*obj_.argument(++i), count) || count <= 0)
            return error("ERR count should be greater than 0");
    }
    std::vector<std::string> keys;
    keys.reserve(static_cast<size_t>(numkeys));
    for (size_t i = 2; i < where; ++i) keys.push_back(*obj_.argument(i));
    if (check_cluster_mode(true) && route_keys(keys, result)) return true;
    dao::paged_zset data;
    dao::zset_pop_result out;
    std::string selected_key;
    auto& reply = result.create_object();
    if (!data.mpop(handler_.get_db(), keys, maximum, count, selected_key, out))
        reply.set_error(data.error());
    else if (!out.found) reply.set_type(REDIS_OBJ_NULL_ARRAY);
    else {
        reply.set_type(REDIS_OBJ_ARRAY);
        reply.create_child().set_string(selected_key);
        auto& members = reply.create_child();
        members.set_type(REDIS_OBJ_ARRAY);
        for (const auto& value : out.values) {
            auto& pair = members.create_child();
            pair.set_type(REDIS_OBJ_ARRAY);
            pair.create_child().set_string(value.member);
            emit_score(pair.create_child(), value.score);
        }
    }
    return true;
}

bool redis_zset::sample(redis_coder& result) {
    const auto error = [&](const char* message) {
        result.create_object().set_error(message); return true; };
    const bool has_count = obj_.size() >= 3;
    long long count = 1;
    if (has_count && !parse_redis_integer(*obj_.argument(2), count)) return
        error("ERR value is not an integer or out of range");
    if (count == LLONG_MIN) return error(
        "ERR value is out of range, value must between "
        "-9223372036854775807 and 9223372036854775807");
    if (obj_.size() > 4 || (obj_.size() == 4 && !option_is(*obj_.argument(3),
        "WITHSCORES"))) return error("ERR syntax error");
    const bool scores = obj_.size() == 4;
    if (scores && (count < -LLONG_MAX / 2 || count > LLONG_MAX / 2)) return
        error("ERR value is out of range");
    if (check_cluster_mode(true) && route_keys("ZRANDMEMBER", result)) return
        true;
    dao::paged_zset data;
    dao::zset_sample_result out;
    auto& reply = result.create_object();
    if (!data.sample(handler_.get_db(), *obj_.argument(1), count, out))
        reply.set_error(data.error());
    else if (has_count) emit(reply, out.values, scores);
    else if (!out.found || out.values.empty()) reply.set_null();
    else reply.set_string(out.values.front().member);
    return true;
}

bool redis_zset::select(const char* cmd, redis_coder& result) {
    const auto is = [&](const char* name) { return !strcasecmp(cmd, name); };
    const bool store = is("ZRANGESTORE");
    const bool count = is("ZCOUNT") || is("ZLEXCOUNT");
    const bool remove = !strncasecmp(cmd, "ZREMRANGE", 9);
    const size_t source = store ? 2 : 1;
    std::vector<std::string> keys{*obj_.argument(source)};
    if (store) keys.push_back(*obj_.argument(1));
    if (check_cluster_mode(true) && route_keys(keys, result)) return true;
    auto& reply = result.create_object();
    const auto error = [&](const char* message) { reply.set_error(message);
        return true; };
    zset_query query;
    bool auto_mode = is("ZRANGE") || store;
    const bool auto_direction = auto_mode;
    query.reverse = is("ZREVRANGE") || is("ZREVRANGEBYSCORE") || is(
        "ZREVRANGEBYLEX");
    if (is("ZRANGEBYSCORE") || is("ZREVRANGEBYSCORE") || is("ZCOUNT") || is(
        "ZREMRANGEBYSCORE")) query.mode = zrange_mode::score;
    if (is("ZRANGEBYLEX") || is("ZREVRANGEBYLEX") || is("ZLEXCOUNT") || is(
        "ZREMRANGEBYLEX")) query.mode = zrange_mode::lex;
    bool scores = false;
    for (size_t i = source + 3; i < obj_.size(); ++i) {
        const auto& option = *obj_.argument(i);
        if (option_is(option, "WITHSCORES") && !store && !count && !remove)
            scores = true;
        else if (option_is(option, "LIMIT") && !count && !remove && i + 2 <
            obj_.size()) {
            if (!parse_redis_integer(*obj_.argument(i + 1), query.offset) ||
                !parse_redis_integer(*obj_.argument(i + 2), query.limit))
                return error("ERR value is not an integer or out of range");
            i += 2;
        } else if (option_is(option, "REV") && auto_direction && !query.reverse)
            query.reverse = true;
        else if ((option_is(option, "BYSCORE") || option_is(option, "BYLEX")) &&
            auto_mode) {
            query.mode = option_is(option, "BYSCORE") ? zrange_mode::score :
                zrange_mode::lex;
            auto_mode = false;
        } else return error("ERR syntax error");
    }
    if (query.mode == zrange_mode::rank && query.limit != -1)
        return error("ERR syntax error, LIMIT is only supported in "
            "combination with either BYSCORE or BYLEX");
    if (query.mode == zrange_mode::lex && scores)
        return error("ERR syntax error, WITHSCORES not supported in "
            "combination with BYLEX");
    const auto& first = *obj_.argument(source + 1);
    const auto& last = *obj_.argument(source + 2);
    if (query.mode == zrange_mode::rank) {
        if (!parse_redis_integer(first, query.start) ||
            !parse_redis_integer(last, query.end))
            return error("ERR value is not an integer or out of range");
    } else if (query.mode == zrange_mode::score) {
        const auto& low = query.reverse ? last : first;
        const auto& high = query.reverse ? first : last;
        if (!parse_zscore_bound(low, query.min_score) ||
            !parse_zscore_bound(high, query.max_score))
            return error("ERR min or max is not a float");
    } else if (!parse_zlex_bound(query.reverse ? last : first, query.min_lex) ||
        !parse_zlex_bound(query.reverse ? first : last, query.max_lex))
        return error("ERR min or max not valid string range item");
    dao::paged_zset data(handler_.page_pipeline_scope_);
    auto& database = handler_.get_db();
    long long number = 0;
    bool ok;
    if (store) ok = data.store_range(database, keys[0], keys[1], query, number);
    else if (count) ok = data.count(database, keys[0], query, number);
    else if (remove) ok = data.remove_range(database, keys[0], query, number);
    else {
        std::vector<zset_member> values;
        if (!data.select(database, keys[0], query, values))
            reply.set_error(data.error());
        else if (scores) emit_scored(reply, values, handler_.client().protocol());
        else emit(reply, values, false);
        return true;
    }
    if (!ok) reply.set_error(data.error()); else reply.set_number(number);
    return true;
}

bool redis_zset::add(bool increment, redis_coder& result) {
    dao::zadd_options options;
    options.incr = increment;
    size_t first = 2;
    if (!increment) for (; first < obj_.size(); ++first) {
        const auto& option = *obj_.argument(first);
        if (option_is(option, "NX")) options.nx = true;
        else if (option_is(option, "XX")) options.xx = true;
        else if (option_is(option, "GT")) options.gt = true;
        else if (option_is(option, "LT")) options.lt = true;
        else if (option_is(option, "CH")) options.ch = true;
        else if (option_is(option, "INCR")) options.incr = true;
        else break;
    }
    const auto error = [&](const char* message) {
        result.create_object().set_error(message); return true; };
    if (first == obj_.size() || (obj_.size() - first) % 2) return error(
        "ERR syntax error");
    if (options.nx && options.xx) return error(
        "ERR XX and NX options at the same time are not " "compatible");
    if ((options.nx && (options.gt || options.lt)) || (options.gt &&
        options.lt))
        return error("ERR GT, LT, and/or NX options at the same time are "
            "not compatible");
    if (options.incr && obj_.size() - first > 2) return error(
        "ERR INCR option supports a single increment-element pair");
    const auto pairs = (obj_.size() - first) / 2;
    double score = 0;
    std::vector<zset_member> values;
    if (pairs > 1) values.reserve(pairs);
    for (size_t i = first; i < obj_.size(); i += 2) {
        if (!parse_redis_double(*obj_.argument(i), score)) return error(
            "ERR value is not a valid float");
        if (pairs > 1) values.push_back({*obj_.argument(i + 1), score});
    }
    if (check_cluster_mode(true) && route_keys(increment ? "ZINCRBY" : "ZADD",
        result)) return true;
    auto* scope = handler_.page_pipeline_scope_;
    if (scope && pairs == 1 && !options.incr)
        scope->track_zadd_member(obj_.argument(first + 1));
    dao::paged_zset data(scope);
    dao::zadd_result out;
    auto& reply = result.create_object();
    auto& db = handler_.get_db();
    const auto& key = *obj_.argument(1);
    const bool ok = pairs == 1 ?
        data.add_one(db, key, *obj_.argument(first + 1), score, options, out) :
        data.add(db, key, values, options, out);
    if (!ok)
        reply.set_error(data.error());
    else if (options.incr) {
        if (out.processed) emit_score(reply, out.score); else reply.set_null();
    } else reply.set_number(options.ch ? out.changed : out.added);
    return true;
}
}
