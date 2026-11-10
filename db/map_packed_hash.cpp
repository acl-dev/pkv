#include "map_store.h"
#include "packed_hash_store.h"
#include "coder/compact_hash.h"
namespace pkv {
bool map_store::refresh_packed_hash() {
    packed_hash::root metadata;
    if (packed_hash::decode_root(entry_->value,metadata) != map_page_status::ok)
        return fail(status::invalid);
    root_ = map_root(); root_.kind = map_kind::hash;
    root_.elements = metadata.elements; root_.expires_at_ms = metadata.expires_at_ms;
    root_.last_id = metadata.groups(); dirty_ = false;
    packed_metadata_valid_ = true;
    return true;
}
bool map_store::packed_callback(const packed_operation& callback) {
    if (error_ != status::ok) return false;
    packed_hash_store store(transaction_,owner_,entry_);
    if (packed_metadata_valid_) store.open_validated(root_);
    else if (!store.open()) return fail(store.error());
    if (!callback(store)) {
        transaction_.abort();
        return store.error() == status::ok ? false : fail(store.error());
    }
    if (entry_->action == db::update_action::erase) {
        packed_metadata_valid_ = false;
        root_ = map_root(); root_.kind = kind_; dirty_ = false; return true;
    }
    if (has_packed_hash()) {
        // The context validated this root and publishes only valid metadata.
        // Reuse its owned scalars instead of decoding the same root again.
        root_.kind = map_kind::hash; root_.elements = store.size();
        root_.expires_at_ms = store.expiry(); root_.last_id = store.root_.groups();
        root_.tree = 0; root_.bucketed = root_.grouped = false;
        packed_metadata_valid_ = true;
        dirty_ = false; return true;
    }
    packed_metadata_valid_ = false;
    if (!decoded(map_page_codec::decode_root(entry_->value,root_))) return false;
    dirty_ = false; return true;
}
bool map_store::build_hash_auto(const hash_field_refs& fields,int& added) {
    if (kind_ != map_kind::hash || embedded_ || exists() || fields.empty())
        return fail(status::invalid);
    hash_created_ = true;
    if (transaction_.database_.supports_compact_hashes() && fields.size() >= 64 &&
        fields.size() <= packed_hash::max_fields) {
        packed_hash_store::fields values;
        for (const auto& item : fields) values[*item.first] = *item.second;
        bool created;
        if (!packed_hash_store::create(transaction_,owner_,values,root_.expires_at_ms,created))
            return fail(status::storage);
        if (created) { added = int(values.size()); return refresh_packed_hash(); }
    }
    return build_hash(fields,added);
}
bool map_store::reduce_hash(bool& changed) {
    changed = false;
    if (kind_ != map_kind::hash || embedded_ || !root_.elements ||
        !transaction_.database_.supports_compact_hashes()) return true;
    const bool packed = has_packed_hash();
    if (root_.elements > compact_hash::max_fields &&
        (!hash_created_ || packed || root_.elements > packed_hash::max_fields)) return true;
    if (root_.elements <= compact_hash::max_fields &&
        !(packed || hash_created_ || hash_shrank_)) return true;
    std::vector<std::pair<std::string,std::string>> items;
    // Probe lengths before materializing optional compact candidates. Large
    // payloads must not be read just to discover that they cannot be packed.
    if (!packed) {
        bool eligible = true;
        size_t payload = 0;
        const size_t item_limit = root_.elements <= compact_hash::max_fields ? 254 : packed_hash::max_item_bytes;
        const size_t payload_limit = root_.elements <= compact_hash::max_fields ? compact_hash::payload_limit : SIZE_MAX;
        if (!visit([&](const std::string& field,const std::string&) {
            uint64_t bytes; bool found;
            if (field.size() > item_limit) { eligible = false; return false; }
            if (!length(field,bytes,found) || !found) { eligible = false; return false; }
            if (bytes > item_limit || field.size() + size_t(bytes) + 1 > payload_limit - payload) {
                eligible = false; return false;
            }
            payload += field.size() + size_t(bytes) + 1; return true;
        },false)) return false;
        if (error_ != status::ok) return false;
        if (!eligible) return true;
    }
    const auto collect = [&](std::string&& field,std::string&& value) {
        items.emplace_back(std::move(field),std::move(value)); return true;
    };
    if (!visit_all(std::cref(collect),true) || items.size() != root_.elements)
        return error_ == status::ok ? fail(status::invalid) : false;
    std::sort(items.begin(),items.end());
    if (root_.elements <= compact_hash::max_fields) {
        auto bytes = compact_hash::encode(items.begin(),items.end(),items.size(),root_.expires_at_ms);
        if (bytes.empty()) return true;
        if (!transaction_.reset_pages(owner_)) return fail(status::storage);
        entry_->value.swap(bytes); entry_->action = db::update_action::write;
        entry_->maintenance_only = false; dirty_ = false; changed = true; return true;
    }
    packed_hash_store::fields values;
    for (auto& item : items) values.emplace(std::move(item.first),std::move(item.second));
    if (!packed_hash_store::create(transaction_,owner_,values,root_.expires_at_ms,changed))
        return fail(status::storage);
    return !changed || refresh_packed_hash();
}
bool map_store::finish_hash() {
    if (error_ != status::ok) return false;
    if (dirty_ || (has_packed_hash() && entry_->action == db::update_action::write &&
        !entry_->maintenance_only)) {
        bool changed;
        if (!reduce_hash(changed)) return false;
        if (changed) return true;
    }
    return finish();
}
bool map_store::clone_packed_hash(map_store& destination,bool& copied) {
    packed_hash_store source(transaction_,owner_);
    packed_hash_store::fields values;
    if (!source.open() || !source.collect(values)) {
        destination.transaction_.abort(); return fail(source.error());
    }
    if (destination.transaction_.database_.supports_compact_hashes()) {
        bool created;
        if (!packed_hash_store::replace(destination.transaction_,destination.owner_,values,
            source.expiry(),source.root_.bits,created)) {
            transaction_.abort(); return destination.fail(status::storage);
        }
        if (created) { copied = true; return destination.refresh_packed_hash(); }
    }
    destination.initialize(source.expiry());
    destination.entry_->value.clear();
    hash_field_refs refs;
    for (const auto& item : values) refs.emplace_back(&item.first,&item.second);
    int added;
    if (!destination.build_hash(refs,added) || added != int(values.size())) {
        transaction_.abort(); return destination.fail(status::invalid);
    }
    copied = true; return true;
}
}
