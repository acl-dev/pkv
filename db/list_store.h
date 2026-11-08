#pragma once
#include "page_transaction.h"
#include "coder/list_page.h"
#include "common/redis_list_util.h"
#include <unordered_set>

namespace pkv {

// Typed List operations inside one existing logical-key transaction. Values
// are packed into bounded pages, with fragmentation for individual big members.
class list_store {
public:
    enum class status {
        ok, not_paged, invalid, unsupported, storage, too_large,
        invalid_argument
    };
    static const uint64_t max_member_bytes = 512ULL * 1024 * 1024;

    list_store(page_transaction& transaction, const std::string& owner);
    ~list_store();
    list_store(const list_store&) = delete;
    list_store& operator=(const list_store&) = delete;
    bool open();
    // Reinitialize an expired/replaced value, reclaiming all its old pages in
    // the same native transaction. Caller handles type/expiry policy.
    void initialize(int64_t expires_at_ms = -1);
    bool exists() const { return root_.elements != 0; }
    const list_root& metadata() const { return root_; }
    status error() const { return error_; }

    bool push(const std::vector<std::string>& values, bool left) {
        return push(list_values_view(values.data(), values.size()), left);
    }
    bool push(list_values_view values, bool left);
    bool pop(uint64_t count, bool left, std::vector<std::string>* out);
    bool pop_one(bool left, std::string& out);
    bool range(long long start, long long end, std::vector<std::string>& out);
    bool index(long long position, std::string& out, bool& found);
    bool trim(long long start, long long end);
    bool positions(const std::string& value, const list_search& options,
        std::vector<long long>& out);
    bool replace(long long position, const std::string& value, bool& found);
    bool insert(bool before, const std::string& pivot,
        const std::string& value, long long& length);
    bool remove(long long count, const std::string& value, long long& removed);
    bool move_to(list_store& destination, bool from_left, bool to_left,
        std::string& value, bool& found);
    bool clone_to(list_store& destination, bool& copied);
    bool clear();
    bool set_expiry(int64_t expires_at_ms);
    bool finish();

private:
    page_transaction& transaction_;
    const std::string owner_;
    db::batch_entry* entry_;
    list_root root_;
    status error_ = status::ok;
    bool dirty_ = false;

    struct position {
        uint64_t page = 0;
        size_t slot = 0;
    };
    struct span {
        position first;
        position last; // Inclusive fragment position.
    };

    class cursor {
    public:
        cursor(list_store& store, bool reverse);
        bool member(std::string* out, const std::string* match = nullptr,
            bool* matched = nullptr, span* where = nullptr);
    private:
        list_store& store_;
        const bool reverse_;
        list_page page_;
        size_t position_ = 0;
        uint64_t next_ = 0;
        uint64_t previous_ = 0;
        std::unordered_set<uint64_t> visited_;
        position current_;
        const list_fragment* fragment();
    };

    class segment_writer {
    public:
        segment_writer(list_store& store, uint64_t id, uint64_t previous);
        bool fragment(const list_fragment& value);
        bool value(const std::string& value);
        bool finish(uint64_t next);
        uint64_t first() const { return nonempty_ ? first_ : 0; }
        uint64_t last() const { return nonempty_ ? page_.id : 0; }
    private:
        list_store& store_;
        list_page page_;
        const uint64_t first_;
        size_t used_ = list_page_codec::page_header_bytes;
        bool nonempty_ = false;
        bool grow();
    };

    bool locate(uint64_t index, span& out);
    bool compact_index(uint64_t offset, list_page_update& result,
        bool& handled, const std::string* replacement = nullptr);
    bool compact_range(uint64_t skip, uint64_t count, bool reverse,
        std::vector<std::string>& values, bool& handled);
    bool splice(position begin, position end, const std::string* value,
        uint64_t removed);
    bool neighbors(const list_page& first, const list_page& last,
        uint64_t new_first, uint64_t new_last);
    bool fail(status value);
    bool inspect(uint64_t id, std::string& scratch,
        db::page_inspector visit, void* context);
    bool read_page(uint64_t id, list_page& out);
    bool check_links(uint64_t id, uint64_t previous, uint64_t next);
    bool prepare_push(list_values_view values, bool left,
        list_page& page, bool& applied, size_t fill_target);
    bool pop_values(uint64_t count, bool left, std::vector<std::string>* out,
        std::string* single);
    bool prepare_pop(bool left, std::string* out,
        list_page& page, bool& applied, uint64_t count = 1,
        std::vector<std::string>* values = nullptr, uint64_t removed_neighbor = 0,
        uint32_t* consumed = nullptr);
    bool write_page(const list_page& page);
    bool counter();
    bool allocate(list_page& page);
    bool grow(list_page& page, bool left);
    bool append(list_page& page, const std::string& value, bool left,
        size_t fill_target);
    bool take(list_page& page, bool left, std::string* out);
    bool advance(list_page& page, bool left);
    static size_t used(const list_page& page);
};

} // namespace pkv
