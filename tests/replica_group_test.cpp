#include "stdafx.h"
#include "replica_group_types.h"
#include "db/replica_group.h"
#include <chrono>

static void budgets(bool tbb) {
    unsigned failed = 0, succeeded = 0;
    for (long allowance = 0; allowance < 512; ++allowance) {
        group_fixture files(tbb);
        const auto before = files.images();
        std::vector<watch_token> tokens;
        // Retain one independent token per DB.
        for (auto database : files.databases) {
            std::vector<watch_token> one;
            require(database->watch({family_key}, one) == db::batch_status::ok,
                "group retained WATCH"); tokens.push_back(one[0]);
        }
        family_budget = allowance;
        const auto status = files.group->apply(*files.pool);
        family_budget = -1;
        if (status == db::batch_status::ok) {
            ++succeeded; files.complete();
            require(files.trace.prepared == 3 && files.trace.published == 3,
                "group did not prepare/publish all DBs");
            for (const auto& token : tokens)
                require(!token->valid(unix_time_ms()), "group WATCH lost");
        } else {
            ++failed;
            require(status == db::batch_status::error &&
                !files.trace.published && files.images() == before &&
                !files.events.calls, "group allocation prefix escaped");
            for (const auto& token : tokens)
                require(token->valid(unix_time_ms()), "rollback changed WATCH");
        }
    }
    require(failed && succeeded, "group budgets lack both paths");
    std::cout << "Group budgets " << tbb << ": failed=" << failed
        << " succeeded=" << succeeded << '\n';
}
static void errors(bool tbb) {
    for (unsigned failed_at : {1U, 2U, 3U}) {
        group_fixture files(tbb); const auto before = files.images();
        files.trace.failed_at = failed_at;
        require(files.group->apply(*files.pool) == db::batch_status::error &&
            !files.trace.published && files.images() == before,
            "native group preparation failed to roll back");
    }
    group_fixture files(tbb); const auto before = files.images();
    control(files.databases[1]).fail_read = true;
    require(files.group->apply(*files.pool) == db::batch_status::error &&
        !files.trace.prepared, "group root read error ignored");
    control(files.databases[1]).fail_read = false;
    database_pool aliases(files.databases[0], 16, [&](size_t id) {
        return id == 7 ? files.databases[0] : files.databases[2];
    });
    require(files.group->apply(aliases) == db::batch_status::unsupported &&
        !files.trace.prepared && files.images() == before,
        "aliased DB group accepted");
    database_pool unavailable(files.databases[0], 16,
        [](size_t) { return shared_db(); });
    require(files.group->apply(unavailable) == db::batch_status::error &&
        !files.trace.prepared, "missing group DB accepted");
    database_pool small(files.databases[0], 2, [](size_t) {
        require(false, "out of bounds opened a prefix DB"); return shared_db();
    });
    require(files.group->apply(small) == db::batch_status::aborted &&
        !files.trace.prepared && files.images() == before,
        "group DB bounds changed a prefix");
    auto workspace = std::make_shared<page_workspace>(files.databases[1]);
    database_pool overlays(files.databases[0], 16, [&](size_t id) {
        return id == 7 ? workspace : files.databases[2];
    });
    require(files.group->apply(overlays) == db::batch_status::unsupported &&
        !files.trace.prepared && files.images() == before,
        "group workspace accepted");
}
static void invalid_prefix(bool tbb) {
    group_fixture files(tbb); const auto before = files.images();
    std::vector<db::batch_entry> roots;
    roots.emplace_back("ghost", true, "");
    auto missing = page_batch::capture(7, roots,
        {{"ghost", 1, "invalid-prefix", db::update_action::write}});
    const auto input = exec_batch::capture({group_erase(0, family_key),
        missing, group_write(7, "ghost", 1)});
    require(input && input->apply(*files.pool) == db::batch_status::aborted &&
        !files.trace.prepared && files.images() == before,
        "later root rescued missing-owner group prefix");
    std::string value;
    require(!files.databases[1]->get("ghost", value), "ghost owner published");
}
static void history(bool tbb) {
    group_fixture files(tbb);
    std::vector<watch_token> changed, preserved;
    require(files.databases[0]->watch({family_key}, changed) ==
        db::batch_status::ok &&
        files.databases[1]->watch({family_key}, preserved) ==
        db::batch_status::ok, "history WATCH seed");
    auto notify = files.databases[0]->wait_for_keys({family_key});
    const auto input = exec_batch::capture({group_erase(0, family_key),
        group_write(7, family_key, 5, true),
        group_write(0, family_key, 3, true),
        group_write(15, family_key, 2)});
    files.trace.no_allocate = true;
    require(input && input->apply(*files.pool) == db::batch_status::ok,
        "allocation-free group publication failed");
    family_budget = -1;
    key_wait_event event;
    require(!changed[0]->valid(unix_time_ms()) &&
        preserved[0]->valid(unix_time_ms()) && notify->wait(0, event),
        "group lost logical history/maintenance/wake effects");
    require(image(files.databases[0], family_key).root ==
        encoded_root(3, family_deadline) && !files.events.calls,
        "group history root/echo mismatch");
}
static void isolation(bool tbb) {
    group_fixture files(tbb);
    std::promise<void> start;
    auto signal = start.get_future().share();
    auto writer = std::async(std::launch::async, [&] {
        signal.wait(); return files.databases[1]->set("writer", "after");
    });
    files.trace.prepared_all = [&] {
        start.set_value();
        require(writer.wait_for(std::chrono::milliseconds(20)) ==
            std::future_status::timeout, "writer saw prepared group");
    };
    require(files.group->apply(*files.pool) == db::batch_status::ok &&
        writer.get(), "writer failed after group unlock"); files.complete();
}
static void live(bool tbb) {
    group_fixture files(tbb); const auto before = files.images();
    const auto wire = group_frames(*files.group);
    auto pool = std::shared_ptr<database_pool>(files.pool.release());
    cluster_client receiver("", files.databases[0], pool);
    require(receiver.apply(std::make_shared<kv_message>("", "",
        message_oper_ready, 0, 3)), "group READY");
    for (size_t i = 0; i < wire.size(); ++i) {
        rpc_packet packet; packet.page = true; packet.frame = wire[i];
        require(receiver.apply_packet(std::move(packet)), "live group apply");
        if (i + 1 != wire.size()) require(files.images() == before &&
            !files.trace.prepared, "wire prefix changed DBs");
    }
    files.complete();
    cluster_client mismatched("", memory_database(tbb, 2), pool);
    require(mismatched.apply(std::make_shared<kv_message>("", "",
        message_oper_ready, 0, 3)), "mismatch READY");
    for (size_t i = 0; i < wire.size(); ++i) {
        rpc_packet packet; packet.page = true; packet.frame = wire[i];
        require(mismatched.apply_packet(std::move(packet)) ==
            (i + 1 != wire.size()), "mismatched DB0 pool accepted");
    }
    cluster_client clustered("", files.databases[0], pool);
    require(clustered.apply(std::make_shared<kv_message>("", "",
        message_oper_ready, 0, 3)), "cluster READY");
    const auto mode = var_cfg_cluster_mode; var_cfg_cluster_mode = 1;
    for (size_t i = 0; i < wire.size(); ++i) {
        rpc_packet packet; packet.page = true; packet.frame = wire[i];
        require(clustered.apply_packet(std::move(packet)) ==
            (i + 1 != wire.size()), "cluster accepted cross-DB group");
    }
    var_cfg_cluster_mode = mode;
    std::cout << "Live cross-DB framing " << tbb << " passed\n";
}
static void single_database(bool tbb) {
    group_fixture files(tbb);
    const auto before = image(files.databases[0], family_key);
    const auto input = exec_batch::capture({group_write(7, family_key, 1),
        group_erase(7, family_key), group_write(7, family_key, 2)});
    require(input && input->apply(*files.pool) == db::batch_status::ok &&
        !files.trace.prepared && !files.trace.published &&
        image(files.databases[0], family_key) == before &&
        image(files.databases[1], family_key).root ==
            encoded_root(2, family_deadline), "single DB pool replay changed");
}
static void many_databases(bool tbb) {
    group_trace trace;
    std::vector<shared_db> databases;
    std::vector<std::shared_ptr<const page_batch>> batches;
    for (unsigned i = 0; i < 64; ++i) {
        auto database = group_database(tbb, trace, 1);
        require(database->open(""), "many group open");
        databases.push_back(database); batches.push_back(group_write(i, "", 0));
    }
    database_pool pool(databases[0], databases.size(), [&](size_t id) {
        return databases[id];
    });
    const auto input = exec_batch::capture(std::move(batches));
    require(input && input->apply(pool) == db::batch_status::ok &&
        trace.prepared == 64 && trace.published == 64, "iterative DB group");
    for (auto database : databases) index(database, {""});
    std::cout << "Iterative 64-DB group " << tbb << " passed\n";
}
int main() {
    for (bool tbb : {false, true}) {
#ifndef HAS_TBB
        if (tbb) continue;
#endif
        budgets(tbb); errors(tbb); invalid_prefix(tbb); history(tbb);
        isolation(tbb); live(tbb); many_databases(tbb);
        typed_group(tbb); expiry_group(tbb); colliding_stripes(tbb);
        unsupported_group(tbb);
        single_database(tbb);
    }
#ifdef HAS_TBB
    group_fixture mixed(false, true);
    require(mixed.group->apply(*mixed.pool) == db::batch_status::ok,
        "mixed MDB/oneTBB group failed"); mixed.complete();
#endif
    std::cout << "Cross-DB group rollback, history and publication passed\n";
}
