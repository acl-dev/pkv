#include "stdafx.h"
#include "database_pool.h"
#include <stdexcept>

namespace pkv {

struct database_pool::state {
    std::vector<shared_db> databases;
    factory create;
    std::vector<size_t> existing;
    acl::fiber_mutex mutex;

    state(shared_db zero, size_t count, factory make,
        std::vector<size_t> stored)
    : databases(count)
    , create(std::move(make)), existing(std::move(stored))
    {
        if (!count || !zero) {
            throw std::invalid_argument("database pool requires DB 0");
        }
        for (auto index : existing) if (index >= count)
            throw std::invalid_argument("invalid stored database index");
        databases[0] = std::move(zero);
    }
};

database_pool::database_pool(shared_db zero, size_t count, factory create,
    std::vector<size_t> existing)
: state_(new state(std::move(zero), count, std::move(create),
    std::move(existing)))
{}

database_pool::~database_pool() = default;

size_t database_pool::size() const {
    return state_->databases.size();
}

std::vector<shared_db> database_pool::opened() {
    std::vector<shared_db> out;
    if (!state_->mutex.lock()) return out;
    std::unique_lock<acl::fiber_mutex> guard(state_->mutex, std::adopt_lock);
    for (const auto& db : state_->databases) if (db) out.push_back(db);
    return out;
}

std::vector<std::pair<size_t, shared_db>> database_pool::materialized() {
    std::vector<size_t> selected;
    {
        if (!state_->mutex.lock())
            throw std::runtime_error("database pool lock failed");
        std::unique_lock<acl::fiber_mutex> guard(state_->mutex,
            std::adopt_lock);
        selected = state_->existing;
        for (size_t i = 0; i < size(); ++i)
            if (state_->databases[i]) selected.push_back(i);
    }
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()),
        selected.end());
    std::vector<std::pair<size_t, shared_db>> result;
    for (auto index : selected) {
        auto database = get(index);
        if (!database) throw std::runtime_error("stored database open failed");
        result.emplace_back(index, std::move(database));
    }
    return result;
}

bool database_pool::close_for_exit() {
    if (!state_->mutex.lock()) return false;
    std::unique_lock<acl::fiber_mutex> guard(state_->mutex, std::adopt_lock);
    state_->create = {};
    bool ok = true;
    for (const auto& database : state_->databases)
        if (database && !database->close_for_exit()) ok = false;
    return ok;
}

shared_db database_pool::get(size_t index) {
    if (index >= size() || !state_->mutex.lock()) {
        return nullptr;
    }
    std::unique_lock<acl::fiber_mutex> guard(state_->mutex, std::adopt_lock);
    if (!state_->databases[index] && state_->create) {
        try {
            state_->databases[index] = state_->create(index);
        } catch (const std::exception& error) {
            logger_error("open logical DB failed: %s", error.what());
        }
    }
    return state_->databases[index];
}

} // namespace pkv
