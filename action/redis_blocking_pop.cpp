#include "stdafx.h"
#include "redis_blocking_pop.h"
#include "redis_handler.h"
#include "dao/paged_list.h"
#include "dao/paged_zset.h"
#include "common/redis_double.h"
#include "common/list_block.h"
#include "common/blocked_client.h"
#include "common/command_keys.h"
#include <climits>
namespace pkv {
namespace {
struct pop_result {
    dao::list_result list;
    dao::zset_pop_result zset;
    bool found = false;
};
void emit_score(redis_object& reply, double score) {
    reply.set_string(format_redis_double(score));
    reply.response_as(redis_object::response_kind::real);
}
void timed_out(redis_coder& result) {
    result.create_object().set_type(REDIS_OBJ_NULL_ARRAY);
}
void emit(redis_coder& result, const list_block_plan& plan,
    const std::string& key, const pop_result& data) {
    auto& out = result.create_object();
    if (!data.found) {
        out.set_type(plan.move ? REDIS_OBJ_NULL_STRING : REDIS_OBJ_NULL_ARRAY);
        return;
    }
    if (plan.sorted) {
        out.set_type(REDIS_OBJ_ARRAY);
        out.create_child().set_string(key);
        if (!plan.nested) {
            out.create_child().set_string(data.zset.values.front().member);
            emit_score(out.create_child(), data.zset.values.front().score);
        } else {
            auto& values = out.create_child(); values.set_type(REDIS_OBJ_ARRAY);
            for (const auto& value : data.zset.values) {
                auto& pair = values.create_child();
                pair.set_type(REDIS_OBJ_ARRAY);
                pair.create_child().set_string(value.member);
                emit_score(pair.create_child(), value.score);
            }
        }
        return;
    }
    if (plan.move) { out.set_string(data.list.values.front()); return; }
    out.set_type(REDIS_OBJ_ARRAY);
    out.create_child().set_string(key);
    if (!plan.nested) out.create_child().set_string(data.list.values.front());
    else {
        auto& values = out.create_child(); values.set_type(REDIS_OBJ_ARRAY);
        for (const auto& value : data.list.values)
            values.create_child().set_string(value);
    }
}
bool probe(redis_handler& handler, const list_block_plan& plan,
    const key_wait_token& token, key_wait_event* event,
    std::string& key, pop_result& out, std::string& error,
    const std::function<bool()>& admitted = {}) {
    dao::paged_list data;
    dao::paged_list::key_predicate eligible;
    dao::paged_list::before_probe before;
    if (token && !event) eligible = [&](const std::string& candidate) {
        return token->first(candidate);
    };
    if (event) {
        before = [&] {
            if (!token->refresh(*event)) return false;
            return !event->ready || !admitted || admitted();
        };
        eligible = [&](const std::string& candidate) {
            return event->ready || token->first(candidate);
        };
    }
    if (plan.sorted) {
        dao::paged_zset sorted;
        const auto ok = sorted.mpop_checked(handler.get_db(), plan.keys,
            !plan.left, plan.count, key, out.zset, eligible, before);
        if (!ok) error = sorted.error();
        out.found = out.zset.found;
        return ok;
    }
    const auto ok = plan.move ? data.move_checked(handler.get_db(),
        plan.keys.front(), plan.destination, plan.left, plan.to_left, out.list,
        eligible, before) : data.mpop_checked(handler.get_db(), plan.keys,
        plan.left, plan.count, key, out.list, eligible, before);
    if (!ok) error = data.error();
    out.found = out.list.found;
    return ok;
}
bool cancelled(redis_handler& handler, blocked_client& control,
    redis_coder& result) {
    if (control.status() == blocked_client::state::disconnected)
        handler.disconnected();
    if (control.status() == blocked_client::state::error)
        result.create_object().set_error(
            "UNBLOCKED client unblocked via CLIENT UNBLOCK");
    else timed_out(result);
    return true;
}
}
bool redis_blocking_pop::exec(const char* command, redis_coder& result) {
    try { return run(command, result); }
    catch (const std::bad_alloc&) {
        // Blocking roots flush earlier replies; EXEC uses a private response.
        result.clear();
        result.create_object().set_error("OOM blocking pop allocation failed");
        return true;
    }
}
bool redis_blocking_pop::run(const char* command, redis_coder& result) {
    list_block_plan plan;
    std::vector<const std::string*> args;
    for (size_t i = 0; i < obj_.size(); ++i) args.push_back(obj_.argument(i));
    if (check_cluster_mode(true)) {
        auto borrowed = args;
        if (command_keys(borrowed)) {
            std::vector<std::string> keys;
            for (const auto* key : borrowed) keys.push_back(*key);
            if (route_keys(keys, result)) return true;
        }
    }
    if (!plan.parse(command, args)) {
        result.create_object().set_error(plan.error); return true;
    }
    std::vector<const std::string*>().swap(args);
    std::vector<std::string> route;
    if (check_cluster_mode(true)) {
        route = plan.keys;
        if (plan.move) route.push_back(plan.destination);
    }
    if (route_keys(route, result)) return true;
    const auto start = blocking_clock_ms();
    const auto deadline = plan.timeout_ms > LLONG_MAX - start ? LLONG_MAX :
        start + plan.timeout_ms;
    try {
        for (;;) {
            auto token = handler_.may_block() ?
                (plan.sorted ? handler_.get_db()->wait_for_zsets(plan.keys) :
                handler_.get_db()->wait_for_lists(plan.keys)) :
                key_wait_token();
            std::string key, error;
            pop_result out;
            if (!probe(handler_, plan, token, nullptr, key, out, error)) {
                result.create_object().set_error(error); return true;
            }
            if (out.found || !handler_.may_block() ||
                (plan.timeout_ms && blocking_clock_ms() >= deadline)) {
                if (!out.found && handler_.may_block()) timed_out(result);
                else emit(result, plan, key, out);
                return true;
            }
            client_block_scope scope(handler_.connection(), token);
            auto& control = scope.control();
            handler_.promote_input();
            if (handler_.input_closed()) control.disconnect();
            for (;;) {
                key_wait_event event;
                bool signalled;
                const auto wait_start = statistics_time_us();
                {
                    execution_pause pause;
                    do {
                        const auto left = deadline - blocking_clock_ms();
                        const auto timeout = !plan.timeout_ms ? -1 :
                            static_cast<int>(std::min<long long>(INT_MAX,
                                std::max(0LL, left)));
                        signalled = token->wait(timeout, event);
                    } while (!signalled && control.waiting() &&
                        !acl::fiber::self_killed() && plan.timeout_ms &&
                        blocking_clock_ms() < deadline);
                }
                handler_.blocked_time(statistics_time_us() - wait_start);
                if (acl::fiber::self_killed()) control.disconnect();
                if (!control.begin()) return cancelled(handler_, control,
                    result);
                if (!signalled) {
                    timed_out(result); return true;
                }
                const auto replies = result.get_objects().size();
                auto ok = probe(handler_, plan, token, &event, key, out,
                    error, [&] {
                    return handler_.recheck(obj_, result) &&
                        !route_keys(route, result);
                });
                if (result.get_objects().size() != replies) return true;
                // A cancellation hint is not a Redis producer event. Type
                // changes alone must leave an already blocked client asleep.
                if (!ok && event.source && !event.ready &&
                    error.compare(0, 9, "WRONGTYPE") == 0) ok = true;
                if (!ok) {
                    result.create_object().set_error(error); return true;
                }
                if (out.found) { emit(result, plan, key, out); return true; }
                if (!event.source || !event.ready) {
                    if (event.source) token->miss(event);
                    if (!control.park()) return cancelled(handler_, control,
                        result);
                    continue;
                }
                break;
            }
            // The reference requeues an unsuccessful re-execution at the tail.
        }
    } catch (const std::bad_alloc&) {
        result.clear();
        result.create_object().set_error("OOM blocking pop allocation failed");
    }
    return true;
}
} // namespace pkv
