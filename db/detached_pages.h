#pragma once
#include "db.h"
#include "page_transaction.h"
#include <utility>
#include <unordered_map>

namespace pkv {
template<class Map>
std::string* detached_page_value(Map& values, uint64_t id) {
    return values.get(id);
}
template<class... Args>
std::string* detached_page_value(std::unordered_map<Args...>& values, uint64_t id) {
    const auto found = values.find(id);
    return found == values.end() ? nullptr : &found->second;
}

// A cold handle owns a whole native map, never copies its untouched pages.
// Native root frames must roll back before this handle restores the map.
template<class Map> class detached_pages : public db::prepared_pages {
    Map* original_;
    Map values_;
    std::vector<std::pair<std::string*, std::string>> replacements_;
    std::vector<uint64_t> added_, erased_;
    bool prepared_ = false, published_ = false;
public:
    explicit detached_pages(Map* original) : original_(original) {
        static_assert(noexcept(std::declval<Map&>().swap(
            std::declval<Map&>())), "detached map swap must not throw");
        if (original_) original_->swap(values_);
    }
    ~detached_pages() override {
        if (published_) return; // The original owner may no longer exist.
        for (auto id : added_) values_.erase(id);
        if (original_) original_->swap(values_);
    }
    detached_pages(const detached_pages&) = delete;
    detached_pages& operator=(const detached_pages&) = delete;
    std::unique_ptr<db::prepared_pages> fresh() const override {
        return std::unique_ptr<db::prepared_pages>(new detached_pages(nullptr));
    }
    db::batch_status prepare(const std::vector<page_change>& pages) override {
        if (prepared_ || published_) return db::batch_status::aborted;
        uint64_t previous = 0;
        for (const auto& page : pages) {
            if (!page.id || page.id <= previous ||
                page.value.size() > page_transaction::max_value_bytes ||
                (page.action != db::update_action::write &&
                 page.action != db::update_action::erase) ||
                (page.action == db::update_action::erase &&
                 !page.value.empty())) return db::batch_status::aborted;
            previous = page.id;
        }
        prepared_ = true;
        try {
            replacements_.reserve(pages.size());
            added_.reserve(pages.size()); erased_.reserve(pages.size());
            for (const auto& page : pages) {
                auto* found = detached_page_value(values_, page.id);
                if (page.action == db::update_action::erase) {
                    if (found) erased_.push_back(page.id);
                } else if (found) {
                    replacements_.emplace_back(found, page.value);
                } else {
                    values_.emplace(page.id, page.value);
                    added_.push_back(page.id);
                }
            }
            return db::batch_status::ok;
        } catch (const std::bad_alloc&) { return db::batch_status::error; }
    }
    bool empty_after_publish() const {
        return values_.size() == erased_.size();
    }
    Map& values() { return values_; }
    void publish() noexcept override {
        assert(!published_);
        for (auto& value : replacements_)
            value.first->swap(value.second);
        for (auto id : erased_) values_.erase(id);
        published_ = true;
    }
};
} // namespace pkv
