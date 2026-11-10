#pragma once
#include "coder/packed_hash.h"
#include "map_store.h"
namespace pkv {
// Internal transaction context used by map_store for grouped Hash pages.
// Returned mutation flags/values are provisional until the outer transaction
// commits, just like map_store. Failed mutations poison that transaction.
class packed_hash_store {
public:
    using status = map_store::status;
    using fields = std::map<std::string,std::string>;
    using field_list = std::vector<std::pair<std::string,std::string>>;
    packed_hash_store(page_transaction& tx, const std::string& owner);
    packed_hash_store(const packed_hash_store&) = delete;
    packed_hash_store& operator=(const packed_hash_store&) = delete;
    static bool create(page_transaction& tx, const std::string& owner,
        const fields& values, int64_t deadline, bool& created);
    bool open();
    bool open_live(long long now = -1);
    status error() const { return error_; }
    uint64_t size() const { return elements_; }
    int64_t expiry() const { return root_.expires_at_ms; }
    bool native() const { return native_; }
    bool get(const std::string& field, std::string& value, bool& found);
    bool length(const std::string& field, uint64_t& bytes, bool& found);
    bool put(const std::string& field, const std::string& value,
        bool only_missing, bool& added);
    bool erase(const std::string& field, bool& removed);
    bool take(const std::string& field, std::string& value, bool& found);
    bool all(field_list& out, bool with_values = true);
    bool select(uint64_t rank, std::string& field, bool& found,
        std::string* value = nullptr);
    bool scan(const scan_options& options, scan_page& out);
    bool rank(const std::string& field, uint64_t& rank, bool& found);
    bool set_expiry(int64_t deadline);
    bool clear();
    bool to_native();
private:
    friend class map_store;
    // Only map_store may borrow its owned key and already validated root.
    packed_hash_store(page_transaction& tx, const std::string& owner,
        db::batch_entry* entry);
    void open_validated(const map_root& metadata);
    page_transaction& tx_;
    const std::string owned_owner_;
    const std::string& owner_;
    db::batch_entry* entry_;
    packed_hash::root root_;
    uint64_t elements_ = 0;
    status error_ = status::not_paged;
    bool native_ = false;
    bool cleared_ = false;
    bool fail(status error);
    bool publish(const packed_hash::root& next);
    bool read(const std::string& field, std::string* value,
        uint64_t& bytes, bool& found);
    bool remove(const std::string& field, std::string* value, bool& found);
    bool collect(fields& out);
    static bool replace(page_transaction& tx, const std::string& owner,
        const fields& values, int64_t deadline, unsigned bits, bool& created);
    template<class Visitor> bool inspect(unsigned group, Visitor& visitor) {
        std::string scratch;
        const auto thunk = [](void* raw, const std::string& bytes) {
            (*static_cast<Visitor*>(raw))(bytes);
        };
        const auto result = tx_.inspect_bound_inline(entry_, group + 1, scratch,
            thunk, &visitor);
        return result == db::lookup_status::found || fail(
            result == db::lookup_status::error ? status::storage : status::invalid);
    }
    template<class Operation> bool with_native(Operation&& operation) {
        map_store store(tx_, owner_);
        if (!store.open() || !operation(store) || !store.finish())
            return fail(store.error() == status::ok ? status::invalid : store.error());
        if (entry_->clear_pages || entry_->action == db::update_action::erase)
            cleared_ = true;
        if (cleared_ && entry_->action == db::update_action::write)
            entry_->clear_pages = true;
        elements_ = store.metadata().elements;
        root_.expires_at_ms = elements_ ? store.metadata().expires_at_ms : -1;
        return true;
    }
};
}
