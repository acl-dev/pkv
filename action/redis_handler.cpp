//
// Created by zsx on 2023/7/23.
//

#include "stdafx.h"
#include "coder/redis_object.h"
#include "coder/redis_coder.h"
#include "db/db_cursor.h"
#include "redis_service.h"
#include "redis_key.h"
#include "redis_handler.h"
#include "db/database_pool.h"
#include "db/page_workspace.h"
#include "common/response_writer.h"
#include "common/connection_input.h"
#include "common/connection_output.h"
#include "common/command_catalog.h"
#include "common/redis_integer.h"

namespace pkv {

#define EQ  !strcasecmp

redis_handler::redis_handler(redis_service& service,
    const std::shared_ptr<database_pool>& databases,
     redis_coder& parser, acl::socket_stream& conn,
     worker_statistics* statistics)
: service_(service)
, databases_(databases)
, db_(databases->get(0))
, activity_(sizeof(redis_handler), client_worker_id())
, parser_(parser)
, conn_(conn)
, builder_(parser.get_cache())
, coder_(parser.get_cache())
{
    statistics_ = statistics ? statistics :
        &service_.statistics().worker(client_worker_id());
    identity_ = service_.permissions().find("default");
    client_.authenticated(service_.permissions().default_login());
    connection_ = service_.clients().add(client_.id(), conn_.sock_handle(),
        &client_, client_inspector(this, &redis_handler::observe));
}

size_t redis_handler::database_count() const {
    return databases_->size();
}

shared_db redis_handler::database(size_t index) {
    if (workspace_ && index == workspace_database_) return workspace_;
    return databases_->get(index);
}

bool redis_handler::select_database(size_t index) {
    auto selected = database(index);
    if (!selected) {
        return false;
    }
    db_ = std::move(selected);
    selected_ = index;
    activity_.database(index);
    return true;
}

redis_handler::~redis_handler() {
    if (pubsub_) service_.pubsub().retire(pubsub_);
    if (output_) output_->stop();
    service_.clients().retire(connection_);
    input_.reset();
}

static const std::string* pipeline_page_key(const redis_object& object) {
    const auto* name = object.argument(0);
    if (!name || name->empty()) return nullptr;
    const auto prefix = name->front();
    if (prefix == 'Z' || prefix == 'z') {
        const auto length = name->size();
        const auto argc = object.size();
        const auto* suffix = name->c_str() + 1;
        if (argc == 4 && ((length == 4 && !strcasecmp(suffix, "ADD")) ||
            (length == 7 && !strcasecmp(suffix, "INCRBY")))) {
            const auto* score = object.argument(2);
            const auto* member = object.argument(3);
            if (score && score->size() <= 128 &&
                member && member->size() <= 1024)
                return object.argument(1);
        }
        if ((argc == 4 || argc == 5) &&
            ((length == 6 && !strcasecmp(suffix, "RANGE")) ||
             (length == 9 && !strcasecmp(suffix, "REVRANGE")))) {
            if (argc == 5) {
                const auto* option = object.argument(4);
                if (!option || option->size() != 10 ||
                    strcasecmp(option->c_str(), "WITHSCORES")) return nullptr;
            }
            const auto* first = object.argument(2);
            const auto* last = object.argument(3);
            long long start, end;
            if (first && last && parse_redis_integer(*first, start) &&
                parse_redis_integer(*last, end) && start >= 0 && end >= start &&
                end - start < db::page_lock_scope::max_cached_range)
                return object.argument(1);
        }
        if (argc == 7 &&
            ((length == 13 && !strcasecmp(suffix, "RANGEBYSCORE")) ||
             (length == 16 && !strcasecmp(suffix, "REVRANGEBYSCORE")))) {
            const auto* option = object.argument(4);
            const auto* offset = object.argument(5);
            const auto* limit = object.argument(6);
            const auto* first = object.argument(2);
            const auto* last = object.argument(3);
            long long skip, count;
            if (option && option->size() == 5 &&
                !strcasecmp(option->c_str(), "LIMIT") && first && last &&
                first->size() <= 128 && last->size() <= 128 && offset && limit &&
                parse_redis_integer(*offset, skip) && skip >= 0 &&
                parse_redis_integer(*limit, count) && count >= 0 &&
                count <= db::page_lock_scope::max_cached_range)
                return object.argument(1);
        }
        if (argc == 4 && length == 6 && !strcasecmp(suffix, "COUNT")) {
            const auto* lower = object.argument(2);
            const auto* upper = object.argument(3);
            if (lower && upper && lower->size() <= 128 && upper->size() <= 128)
                return object.argument(1);
        }
        if ((argc == 3 && length == 6 && !strcasecmp(suffix, "SCORE")) ||
            (argc >= 3 && argc <= 8 && length == 7 &&
                !strcasecmp(suffix, "MSCORE")) ||
            ((argc == 3 || argc == 4) &&
                ((length == 5 && !strcasecmp(suffix, "RANK")) ||
                 (length == 8 && !strcasecmp(suffix, "REVRANK"))))) {
            return object.argument(1);
        }
    }
    if ((prefix == 'S' || prefix == 's') && name->size() == 4 &&
        object.size() == 2 && !strcasecmp(name->c_str() + 1, "POP")) {
        return object.argument(1);
    }
    if (prefix == 'H' || prefix == 'h' || prefix == 'S' || prefix == 's') {
        const auto argc = object.size();
        const auto length = name->size();
        const auto* suffix = name->c_str() + 1;
        const bool hash = prefix == 'H' || prefix == 'h';
        const bool single = argc == 3 && (hash ?
            ((length == 4 && !strcasecmp(suffix, "GET")) ||
             (length == 7 && !strcasecmp(suffix, "EXISTS")) ||
             (length == 7 && !strcasecmp(suffix, "STRLEN"))) :
            (length == 9 && !strcasecmp(suffix, "ISMEMBER")));
        const bool multiple = argc >= 3 && argc <= 8 && (hash ?
            (length == 5 && !strcasecmp(suffix, "MGET")) :
            (length == 10 && !strcasecmp(suffix, "MISMEMBER")));
        if (single || multiple) {
            for (size_t i = 2; i < argc; ++i) {
                const auto* member = object.argument(i);
                if (!member || member->size() > 1024) return nullptr;
            }
            return object.argument(1);
        }
        if (hash && argc == 4 &&
            ((length == 7 && !strcasecmp(suffix, "INCRBY")) ||
             (length == 12 && !strcasecmp(suffix, "INCRBYFLOAT")))) {
            const auto* field = object.argument(2);
            const auto* delta = object.argument(3);
            if (field && field->size() <= 1024 && delta &&
                delta->size() <= (length == 7 ? 20 : 128))
                return object.argument(1);
        }
    }
    if ((prefix == 'Z' || prefix == 'z') && name->size() == 7 &&
        object.size() == 2 && (!strcasecmp(name->c_str() + 1, "POPMIN") ||
            !strcasecmp(name->c_str() + 1, "POPMAX"))) {
        return object.argument(1);
    }
    if ((prefix == 'X' || prefix == 'x') && name->size() == 4 &&
        object.size() == 5 && !strcasecmp(name->c_str() + 1, "ADD")) {
        const auto* id = object.argument(2);
        const auto* field = object.argument(3);
        const auto* value = object.argument(4);
        if (id && field && value && id->size() <= 41 &&
            field->size() <= 1024 && value->size() <= 1024) {
            return object.argument(1);
        }
    }
    if (name->size() == 4 && object.size() == 2 &&
        (((prefix == 'I' || prefix == 'i') && !strcasecmp(name->c_str() + 1, "NCR")) ||
         ((prefix == 'D' || prefix == 'd') && !strcasecmp(name->c_str() + 1, "ECR")))) {
        return object.argument(1);
    }
    if ((prefix == 'S' || prefix == 's') && name->size() == 4 &&
        object.size() == 3 && !strcasecmp(name->c_str() + 1, "ADD")) {
        const auto* member = object.argument(2);
        if (member && member->size() <= 1024) {
            return object.argument(1);
        }
    }
    if ((prefix == 'H' || prefix == 'h' || prefix == 'Z' || prefix == 'z') &&
        name->size() == 4 && (object.size() == 4 ||
            ((prefix == 'H' || prefix == 'h') && object.size() == 6)) &&
        !strcasecmp(name->c_str() + 1, (prefix == 'H' || prefix == 'h') ?
            "SET" : "ADD")) {
        for (size_t i = 2; i < object.size(); ++i) {
            const auto* value = object.argument(i);
            if (!value || value->size() > 1024) return nullptr;
        }
        return object.argument(1);
    }
    if (prefix != 'L' && prefix != 'l' && prefix != 'R' && prefix != 'r')
        return nullptr;
    if (prefix == 'L' || prefix == 'l') {
        const auto argc = object.size();
        const auto length = name->size();
        const auto* suffix = name->c_str() + 1;
        if ((argc == 3 && length == 6 && !strcasecmp(suffix, "INDEX")) ||
            (argc == 4 && length == 4 && !strcasecmp(suffix, "SET"))) {
            const auto* index = object.argument(2);
            const auto* value = argc == 4 ? object.argument(3) : nullptr;
            if (index && index->size() <= 20 &&
                (argc == 3 || (value && value->size() <= 1024)))
                return object.argument(1);
        }
    }
    if (name->size() == 4 && !strcasecmp(name->c_str() + 1, "POP")) {
        if (object.size() == 2) return object.argument(1);
        const auto* count = object.size() == 3 ? object.argument(2) : nullptr;
        long long amount;
        if (count && parse_redis_integer(*count, amount) &&
            amount > 0 && amount <= 32) return object.argument(1);
    }
    if (name->size() == 5 && object.size() == 3 &&
        !strcasecmp(name->c_str() + 1, "PUSH")) {
        const auto* value = object.argument(2);
        if (value && value->size() <= 1024) return object.argument(1);
    }
    return nullptr;
}

bool redis_handler::handle() {
    const auto& objs = parser_.get_objects();
    if (objs.empty()) {
        return connection_->status() == client_connection::state::open;
    }

#if 0
    {
	std::string tmp;
	for (size_t i = 0; i < objs.size(); i++) {
		tmp += "+OK\r\n";
	}
	return conn_.write(tmp.c_str(), tmp.size()) == (int) tmp.size();
    }
#endif

    //if (objs.size() >= 20) { printf(">>>objs=%zd\r\n", objs.size()); }

    constexpr size_t pipeline_reply_budget = 32 * 1024;
    size_t pending_payload = 0;
    const auto process = [&](const redis_object* obj) {
        const auto* name = obj->argument(0);
        bool blocking = false;
        if (name && !name->empty() &&
            (name->front() == 'B' || name->front() == 'b')) {
            const auto* spec = service_.get_command(name->c_str());
            blocking = spec && spec->kind == redis_command_kind::blocking;
            if (blocking) {
                if (!send_pending()) return false;
                pending_payload = 0;
            }
        }
        if (output_ && !blocking) {
            output_reply_ = output_->reserve_reply();
            if (!output_reply_) return false;
        }
        const auto first = builder_.get_objects().size();
        root_statistic_ = service_.statistics().unknown();
        root_executed_ = false;
        suppress_completion_ = false;
        blocked_usec_ = 0;
        transaction_acl_failure_ = false;
        if (!handle_one(*obj)) return false;
        if (!root_executed_ && builder_.get_objects().size() > first &&
            builder_.get_objects().back()->get_type() == REDIS_OBJ_ERROR)
            statistics_->reject(root_statistic_);
        const auto& replies = builder_.get_objects();
        for (size_t i = first; i < replies.size(); ++i) {
            auto& reply = *replies[i];
            if (reply.get_type() == REDIS_OBJ_ERROR) record_error(reply);
            reply.response_protocol(client_.protocol());
            if (!output_) pending_payload += reply.get_buf().size();
        }
        if (output_ && !send_pending()) return false;
        return true;
    };
    const bool group_pages = !output_ && !var_cfg_cluster_mode &&
        !transaction_.active() && objs.size() > 1;
    size_t index = 0;
    while (index < objs.size() && !quit_) {
        const auto state = connection_->status();
        if (state == client_connection::state::immediate ||
            state == client_connection::state::retired) return false;
        if (state == client_connection::state::after_reply) break;
        // Large flat replies can dominate pipeline memory. Flush between
        // commands, after any page lock from the previous group is released.
        if (pending_payload >= pipeline_reply_budget) {
            if (!send_pending()) return false;
            pending_payload = 0;
        }
        constexpr size_t group_limit = 64;
        const auto* key = group_pages && !transaction_.active() &&
            index + 1 < objs.size() ? pipeline_page_key(*objs[index]) :
            nullptr;
        size_t end = index + 1;
        while (key && end < objs.size() && end - index < group_limit) {
            const auto* next = pipeline_page_key(*objs[end]);
            if (!next || *next != *key) break;
            ++end;
        }
        if (end == index + 1) {
            if (!process(objs[index++])) return false;
            continue;
        }
        execution_guard admission;
        db::page_lock_scope scope(*db_, *key);
        struct reset_scope {
            db::page_lock_scope*& pointer;
            ~reset_scope() { pointer = nullptr; }
        } reset{page_pipeline_scope_};
        page_pipeline_scope_ = &scope;
        do {
            if (!process(objs[index++])) return false;
        } while (index < end && !quit_ &&
            pending_payload < pipeline_reply_budget &&
            connection_->status() == client_connection::state::open);
    }

    return send_pending();
}
bool redis_handler::send_pending() {
    response_buffer_cache::frame frame(response_buffer_);
    auto& buf = frame.bytes();
    if (!builder_.take_frame(buf)) {
        builder_.clear();
        return false;
    }

    builder_.clear();

    if (buf.empty() && !output_reply_) {
        return connection_->status() == client_connection::state::open;
    }
    // A client can fill its send window before it starts reading a pipeline.
    // Keep draining input while the owner sends a large reply. Only the owner
    // parses and executes commands, so replies remain in request order.
    constexpr size_t duplex_reply_size = 32 * 1024;
    if (!input_ && buf.size() >= duplex_reply_size &&
        connection_->status() == client_connection::state::open &&
        acl_fiber_running()) promote_input();

    if (output_) {
        const auto ticket = output_reply_ ? output_reply_ :
            output_->reserve_reply();
        output_reply_ = 0;
        if (!ticket || !output_->fulfil_reply(ticket,
            std::make_shared<const std::string>(std::move(buf)))) return false;
        return output_->wait(ticket) && !quit_ &&
            connection_->status() == client_connection::state::open;
    }

    //if (objs.size() >= 20) { printf("reply len=%zd\r\n", buf.size()); }
    //printf(">>>buf=[%s]\r\n", buf.c_str());

    activity_.sending(buf);
    size_t written = 0;
    const auto complete = response_write(buf.size(), written,
        [&](size_t offset, size_t size) {
            const auto count = conn_.write(buf.data() + offset, size);
            return count > 0 ? static_cast<size_t>(count) : size_t(0);
        });
    activity_.sent(written);
    statistics_->sent(written);
    return complete && !quit_ &&
        connection_->status() == client_connection::state::open;
}

bool redis_handler::handle_one(const redis_object& object) {
    const auto* command = object.argument(0);
    if (!command || command->empty()) {
        transaction_.fail();
        builder_.create_object().set_error("ERR empty or invalid command");
        return true;
    }
    const char* cmd = command->c_str();
    size_t argument_bytes = command->size();
    for (size_t i = 1; i < object.size(); ++i) {
        const auto* argument = object.argument(i);
        if (!argument) {
            transaction_.fail();
            builder_.create_object().set_error("ERR invalid command argument");
            return true;
        }
        argument_bytes += argument->size();
    }
    // Command specs have the lifetime of the service. Comparing the full
    // string also keeps embedded NULs on the invalid-command path.
    const auto* spec = last_command_;
    if (!spec || *command != spec->name) {
        spec = command->find('\0') == std::string::npos ?
            service_.get_command(cmd) : nullptr;
        last_command_ = spec;
    }
    const auto* label = spec && spec->name[0] != 'C' &&
        (spec->name[0] != 'A' || std::strcmp(spec->name, "ACL")) &&
        (spec->name[0] != 'P' || std::strcmp(spec->name, "PUBSUB")) ?
        spec->name : command_label(object, spec);
    activity_.command(label, argument_bytes);
    root_statistic_ = spec && label == spec->name ? spec->statistic :
        service_.statistics().index(label);
    if (spec && !spec->valid_arity(object.size())) {
        transaction_.fail();
        std::string name(*command);
        for (char& ch : name) {
            const auto c = static_cast<unsigned char>(ch);
            ch = c >= 32 && c < 127 && c != '\'' ?
                static_cast<char>(std::tolower(c)) : '?';
        }
        builder_.create_object().set_error(
            "ERR wrong number of arguments for '" + name + "' command");
        return true;
    }
    if (!spec) {
        transaction_.fail();
        builder_.create_object().set_error("ERR unknown command");
        return true;
    }
    using kind = redis_command_kind;
    const auto category = spec->kind;
    if (category == kind::client && !client_command_valid(object))
        return true;
    if (category == kind::acl && !acl_command_valid(object)) return true;
    if (category == kind::pubsub && !std::strcmp(spec->name, "PUBSUB") &&
        !pubsub_command_valid(object)) return true;
    if (category == kind::pubsub && !output_ &&
        std::strstr(spec->name, "SUBSCRIBE") && !send_pending()) return false;
    if (category == kind::config && !config_command_valid(object)) return true;
    const auto authentication_allowed = [&] {
        if (client_.authenticated() || EQ(cmd, "AUTH") || EQ(cmd, "HELLO") ||
            EQ(cmd, "QUIT") || EQ(cmd, "RESET") ||
            service_.permissions().default_login()) return true;
        if (EQ(cmd, "EXEC")) {
            clear_transaction(); clear_watches();
            builder_.create_object().set_error("EXECABORT Transaction "
                "discarded because of: NOAUTH Authentication required.");
        } else {
            transaction_.fail();
            builder_.create_object().set_error(
                "NOAUTH Authentication required.");
        }
        return false;
    };
    // Connection-only commands can stop another connection while it owns the
    // KV gate. Queued CLIENT commands still execute inside the EXEC admission.
    if (category == kind::client && !transaction_.active()) {
        if (!authentication_allowed() || !acl_permit(object, builder_))
            return true;
        if (pubsub_ && !pubsub_permit(*spec, label)) return true;
        return execute_one(object, builder_, spec);
    }
    const bool exclusive = category == kind::acl || category == kind::config
        || category == kind::info || (transaction_.active() ?
            category == kind::exec && object.size() == 1 :
            (category == kind::move && object.size() == 3) ||
            (category == kind::keys && object.size() == 2) ||
            ((category == kind::dbsize || category == kind::randomkey) &&
                object.size() == 1));
    execution_guard admission(exclusive);
    if (!authentication_allowed()) return true;
    if (!acl_permit(object, builder_)) {
        transaction_.fail(); return true;
    }
    if ((spec->writes || category == kind::exec) && reject_readonly(*spec))
        return true;
    if (pubsub_ && !pubsub_permit(*spec, label)) {
        transaction_.fail(); return true;
    }
    if (!spec->handler) {
        const auto start = statistics_time_us();
        const auto before = builder_.get_objects().size();
        if (EQ(cmd, "WATCH")) {
            const auto ok = watch_keys(object);
            completed(root_statistic_, start, builder_, before);
            return ok;
        }
        if (EQ(cmd, "UNWATCH") && !transaction_.active()) {
            clear_watches();
            builder_.create_object().set_status("OK");
            completed(root_statistic_, start, builder_, before);
            return true;
        }
        if (transaction_control(cmd)) {
            completed(root_statistic_, start, builder_, before);
            return true;
        }
    }
    if (transaction_.active()) {
        try {
            if (route_queued_command(object)) {
                transaction_.fail(); return true;
            }
            if (transaction_.failed() || !watches_valid()) {
                builder_.create_object().set_status("QUEUED"); return true;
            }
            if (queue_transaction(object) == transaction_queue::result::full)
                builder_.create_object().set_error(
                    "OOM transaction queue limit exceeded");
            else builder_.create_object().set_status("QUEUED");
        } catch (const std::bad_alloc&) {
            transaction_.fail();
            builder_.create_object().set_error(
                "OOM transaction queue allocation failed");
        }
        return true;
    }
    return execute_one(object, builder_, spec);
}

bool redis_handler::execute_one(const redis_object& object,
    redis_coder& result, const redis_command_spec* spec) {
    const auto* command = object.argument(0);
    if (!spec && command) spec = service_.get_command(command->c_str());
    replica_read_guard retain(service_.replica() && spec && !spec->writes);
    const auto metric = executing_ ? statistic(object, spec) : root_statistic_;
    const auto before = result.get_objects().size();
    if (executing_ && !acl_permit(object, result, true)) {
        transaction_acl_failure_ = true;
        errors(result, before); return true;
    }
    const auto start = statistics_time_us();
    if (executing_ && command && EQ(command->c_str(), "UNWATCH"))
        result.create_object().set_status("OK");
    else if (spec && spec->handler) {
        if (!spec->handler(*this, command->c_str(), object, result) &&
            result.get_objects().size() == before)
            result.create_object().set_error("ERR command failed");
    } else result.create_object().set_error("ERR unknown command");
    completed(metric, start, result, before, !executing_ && spec &&
        spec->name[0] == 'C' && !std::strcmp(spec->name, "CLIENT"));
    if (executing_) errors(result, before);
    return true;
}

} // namespace pkv
