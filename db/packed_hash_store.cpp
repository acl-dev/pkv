#include "packed_hash_store.h"
#include "node_write_workspace.h"
#include "coder/record_codec.h"
#include <algorithm>
#include <cstring>
namespace pkv {
packed_hash_store::packed_hash_store(page_transaction& tx,const std::string& owner)
    : tx_(tx), owned_owner_(owner), owner_(owned_owner_), entry_(tx.roots().find(owner)) {}
packed_hash_store::packed_hash_store(page_transaction& tx,const std::string& owner,
    db::batch_entry* entry)
    : tx_(tx), owner_(owner), entry_(entry) {}
void packed_hash_store::open_validated(const map_root& metadata) {
    // map_store validated this owned root, or received metadata just published
    // by this context. The synchronous key callback keeps the bytes stable.
    root_.bits = compact_hash::byte(entry_->value[4]) & 7;
    root_.elements = uint16_t(metadata.elements);
    root_.expires_at_ms = metadata.expires_at_ms;
    const size_t offset = root_.expires_at_ms >= 0 ? 15 : 7;
    std::memcpy(root_.sizes.data(),entry_->value.data() + offset,root_.groups());
    elements_ = root_.elements; cleared_ = entry_->clear_pages;
    error_ = status::ok;
}
bool packed_hash_store::fail(status error) { error_ = error; tx_.abort(); return false; }
bool packed_hash_store::open() {
    if (!entry_) return fail(status::invalid);
    if ((!entry_->found && entry_->action != db::update_action::write) ||
        entry_->action == db::update_action::erase || !packed_hash::matches(entry_->value)) {
        error_ = status::not_paged; return false;
    }
    if (packed_hash::decode_root(entry_->value, root_) != map_page_status::ok)
        return fail(status::invalid);
    elements_ = root_.elements; native_ = false; cleared_ = entry_->clear_pages; error_ = status::ok; return true;
}
bool packed_hash_store::open_live(long long now) {
    if (!open()) return false;
    return root_.expires_at_ms < 0 || root_.expires_at_ms >
        (now < 0 ? unix_time_ms() : now) || clear();
}
bool packed_hash_store::replace(page_transaction& tx,const std::string& owner,
    const fields& values,int64_t deadline,unsigned bits,bool& created) {
    created = false;
    if (values.empty() || values.size() > packed_hash::max_fields || deadline < -1 ||
        bits < packed_hash::min_bits || bits > packed_hash::max_bits) return true;
    std::array<fields,packed_hash::max_groups> groups;
    packed_hash::root metadata; metadata.bits = bits;
    metadata.elements = uint16_t(values.size()); metadata.expires_at_ms = deadline;
    for (const auto& item : values) {
        if (item.first.size() > packed_hash::max_item_bytes ||
            item.second.size() > packed_hash::max_item_bytes) return true;
        const auto group = packed_hash::bucket(item.first,bits);
        if (++metadata.sizes[group] > packed_hash::max_group_fields) return true;
        groups[group].insert(item);
    }
    std::array<std::string,packed_hash::max_groups> pages;
    for (unsigned group = 0; group < metadata.groups(); ++group) {
        if (!metadata.sizes[group]) continue;
        pages[group] = compact_hash::encode(groups[group]);
        if (pages[group].empty()) return true;
    }
    std::string bytes;
    if (packed_hash::encode_root(metadata,bytes) != map_page_status::ok) {
        tx.abort(); return false;
    }
    auto* entry = tx.roots().find(owner);
    if (!entry || !tx.reset_pages(owner)) { tx.abort(); return false; }
    entry->value.swap(bytes); entry->action = db::update_action::write;
    entry->maintenance_only = false;
    for (unsigned group = 0; group < metadata.groups(); ++group)
        if (metadata.sizes[group] && !tx.put(owner,group + 1,std::move(pages[group]))) {
            tx.abort(); return false;
        }
    created = true; return true;
}
bool packed_hash_store::create(page_transaction& tx,const std::string& owner,
    const fields& values,int64_t deadline,bool& created) {
    created = false;
    if (deadline < -1) { tx.abort(); return false; }
    if (!tx.roots().find(owner)) { tx.abort(); return false; }
    const auto bits = packed_hash::creation_bits(values.size());
    return !bits || replace(tx,owner,values,deadline,bits,created);
}
bool packed_hash_store::publish(const packed_hash::root& next) {
    if (packed_hash::encode_root(next,entry_->value) != map_page_status::ok)
        return fail(status::storage);
    root_ = next; elements_ = next.elements;
    if (cleared_) entry_->clear_pages = true;
    entry_->action = db::update_action::write; entry_->maintenance_only = false;
    return true;
}
bool packed_hash_store::read(const std::string& field,std::string* value,
    uint64_t& bytes,bool& found) {
    found = false; bytes = 0;
    if (error_ != status::ok) return false;
    if (native_) return with_native([&](map_store& store) {
        return store.hash_field(field,value,bytes,found);
    });
    if (!elements_) return true;
    const auto group = packed_hash::bucket(field,root_.bits);
    if (!root_.sizes[group]) return true;
    auto visitor = [&](const std::string& encoded) {
        const packed_hash::page_view page(encoded,root_,group);
        compact_hash::member_view item;
        if ((found = page.lookup(field,item))) {
            bytes = item.size(); if (value) value->assign(item.data(),item.size());
        }
    };
    try { return inspect(group,visitor); }
    catch (const std::runtime_error&) { return fail(status::invalid); }
}
bool packed_hash_store::get(const std::string& field,std::string& value,bool& found) {
    uint64_t bytes; return read(field,&value,bytes,found);
}
bool packed_hash_store::length(const std::string& field,uint64_t& bytes,bool& found) {
    return read(field,nullptr,bytes,found);
}
bool packed_hash_store::all(field_list& out,bool with_values) {
    out.clear(); if (error_ != status::ok) return false;
    if (elements_ > out.max_size()) return fail(status::invalid);
    out.reserve(size_t(elements_));
    if (native_) {
        if (!with_native([&](map_store& store) {
            const auto emit = [&](std::string&& field,std::string&& value) {
                out.emplace_back(std::move(field),std::move(value)); return true;
            };
            return store.visit_all(std::cref(emit),with_values);
        })) { out.clear(); return false; }
    } else {
        try {
            for (unsigned group = 0; group < root_.groups(); ++group) {
                if (!root_.sizes[group]) continue;
                auto visitor = [&](const std::string& encoded) {
                    const packed_hash::page_view page(encoded,root_,group);
                    for (unsigned i = 0; i < page.count(); ++i) {
                        const auto item = page.at(i);
                        out.emplace_back(item.first.str(),with_values ? item.second.str() : std::string());
                    }
                };
                if (!inspect(group,visitor)) { out.clear(); return false; }
            }
        } catch (const std::runtime_error&) { out.clear(); return fail(status::invalid); }
    }
    std::sort(out.begin(),out.end());
    if (out.size() != elements_ || std::adjacent_find(out.begin(),out.end(),
        [](const field_list::value_type& a,const field_list::value_type& b) {
            return a.first == b.first;
        }) != out.end()) { out.clear(); return fail(status::invalid); }
    return true;
}
bool packed_hash_store::collect(fields& out) {
    field_list items; if (!all(items)) return false;
    out.clear(); for (auto& item : items) {
        if (!out.emplace(std::move(item.first),std::move(item.second)).second)
            return fail(status::invalid);
    }
    return true;
}
bool packed_hash_store::to_native() {
    if (error_ != status::ok) return false;
    if (native_) return true;
    fields values; if (!collect(values)) return false;
    map_store::hash_field_refs refs; refs.reserve(values.size());
    for (const auto& item : values) refs.emplace_back(&item.first,&item.second);
    map_store store(tx_,owner_); store.initialize(expiry());
    // Disable packed dispatch before a future integrated build_hash can see it.
    entry_->value.clear();
    int added = 0;
    if (store.error() != map_store::status::ok ||
        (!values.empty() && (!store.build_hash(refs,added) || added != int(values.size()))) ||
        !store.finish()) return fail(store.error() == status::ok ? status::invalid : store.error());
    elements_ = store.metadata().elements; native_ = true; return true;
}
bool packed_hash_store::put(const std::string& field,const std::string& value,
    bool only_missing,bool& added) {
    added = false; if (error_ != status::ok) return false;
    if (native_) return with_native([&](map_store& store) { return store.put(field,value,only_missing,added); });
    const auto group = packed_hash::bucket(field,root_.bits);
    detail::node_write_workspace working; auto& encoded = working.buffer();
    bool found = false, eligible = true, changed = false, identical = false;
    packed_hash::page_validation_cache cache(tx_.scope_ && !only_missing);
    if (root_.sizes[group]) {
        auto visitor = [&](const std::string& stored) {
            const packed_hash::page_view page(stored,root_,group,&cache);
            compact_hash::member_view previous;
            if (only_missing) {
                found = page.lookup(field,previous);
                if (found) return;
            }
            bool inserted = false;
            changed = page.put(field,value,encoded,inserted,&identical);
            if (changed) {
                // put already found the insertion position. Reuse its result
                // instead of searching the same validated page a second time.
                found = !inserted;
                eligible = found || elements_ < packed_hash::max_fields;
                // put preserves the validated layout and checks new members.
                if (eligible) cache.remember(identical ? stored : encoded, 0, page.context());
            } else {
                if (!only_missing) found = page.lookup(field,previous);
                eligible = false;
            }
        };
        try { if (!inspect(group,visitor)) return false; }
        catch (const std::runtime_error&) { return fail(status::invalid); }
    } else if (field.size() <= packed_hash::max_item_bytes &&
        value.size() <= packed_hash::max_item_bytes && elements_ < packed_hash::max_fields) {
        encoded = compact_hash::encode({{field,value}}); changed = true;
    } else eligible = false;
    if (only_missing && found) return true;
    if (!eligible) {
        if (field.size() <= packed_hash::max_item_bytes && value.size() <= packed_hash::max_item_bytes &&
            elements_ + !found <= packed_hash::max_fields && root_.bits < packed_hash::max_bits) {
            fields values; if (!collect(values)) return false;
            values[field] = value;
            for (unsigned bits = root_.bits + 1; bits <= packed_hash::max_bits; ++bits) {
                bool rebuilt;
                if (!replace(tx_,owner_,values,expiry(),bits,rebuilt)) return fail(status::storage);
                if (rebuilt) { if (!open()) return false; added = !found; return true; }
            }
        }
        if (!to_native()) return false;
        return with_native([&](map_store& store) { return store.put(field,value,only_missing,added); });
    }
    if (!changed) return fail(status::invalid);
    if (!found) {
        auto next = root_; ++next.sizes[group]; ++next.elements;
        if (!publish(next)) return false;
    } else {
        entry_->action = db::update_action::write; entry_->maintenance_only = false;
        if (cleared_) entry_->clear_pages = true;
    }
    // The root write still publishes WATCH and replication effects.
    if (!identical && !tx_.put(owner_,group + 1,std::move(encoded))) return fail(status::storage);
    added = !found; return true;
}
bool packed_hash_store::remove(const std::string& field,std::string* value,bool& found) {
    found = false; if (error_ != status::ok) return false;
    if (native_) return with_native([&](map_store& store) {
        return value ? store.take(field,*value,found) : store.erase(field,found);
    });
    if (!elements_) return true;
    const auto group = packed_hash::bucket(field,root_.bits);
    if (!root_.sizes[group]) return true;
    std::string encoded, taken;
    auto visitor = [&](const std::string& stored) {
        const packed_hash::page_view page(stored,root_,group);
        compact_hash::member_view previous; unsigned index;
        if (!(found = page.lookup(field,previous,&index))) return;
        if (value) taken.assign(previous.data(),previous.size());
        encoded = page.without(uint16_t(1u << index));
    };
    try { if (!inspect(group,visitor)) return false; }
    catch (const std::runtime_error&) { return fail(status::invalid); }
    if (!found) return true;
    if (elements_ == 1) { if (!clear()) return false; }
    else {
        auto next = root_; --next.sizes[group]; --next.elements;
        if (!publish(next) || !(encoded.empty() ? tx_.erase(owner_,group + 1) :
            tx_.put(owner_,group + 1,std::move(encoded)))) return fail(status::storage);
    }
    if (value) value->swap(taken); return true;
}
bool packed_hash_store::erase(const std::string& field,bool& removed) { return remove(field,nullptr,removed); }
bool packed_hash_store::take(const std::string& field,std::string& value,bool& found) { return remove(field,&value,found); }
bool packed_hash_store::select(uint64_t rank,std::string& field,bool& found,std::string* value) {
    found = false; if (error_ != status::ok) return false;
    if (native_) return with_native([&](map_store& store) { return store.select(rank,field,found,value); });
    if (rank >= elements_) return true;
    unsigned group = 0;
    for (; group < root_.groups(); ++group) {
        if (rank < root_.sizes[group]) break;
        rank -= root_.sizes[group];
    }
    if (group == root_.groups()) return fail(status::invalid);
    auto visitor = [&](const std::string& encoded) {
        const packed_hash::page_view page(encoded,root_,group);
        const auto item = page.at(unsigned(rank));
        field.assign(item.first.data(),item.first.size());
        if (value) value->assign(item.second.data(),item.second.size());
        found = true;
    };
    try { return inspect(group,visitor); }
    catch (const std::runtime_error&) { return fail(status::invalid); }
}
bool packed_hash_store::rank(const std::string& field,uint64_t& rank,bool& found) {
    found = false; rank = 0; if (error_ != status::ok) return false;
    if (native_) return with_native([&](map_store& store) { return store.rank(field,rank,found); });
    const auto group = packed_hash::bucket(field,root_.bits);
    for (unsigned i = 0; i < group; ++i) rank += root_.sizes[i];
    if (!root_.sizes[group]) return true;
    auto visitor = [&](const std::string& encoded) {
        const packed_hash::page_view page(encoded,root_,group);
        compact_hash::member_view value; unsigned index;
        found = page.lookup(field,value,&index); rank += index;
    };
    try { return inspect(group,visitor); }
    catch (const std::runtime_error&) { return fail(status::invalid); }
}
bool packed_hash_store::scan(const scan_options& options,scan_page& out) {
    if (error_ != status::ok) return false;
    if (!options.count) return fail(status::invalid);
    if (native_) return with_native([&](map_store& store) { return store.scan(options,out); });
    scan_page result;
    if (options.cursor >= 256 || !elements_) { out = std::move(result); return true; }
    uint64_t visited = 0;
    try {
        for (unsigned bucket_id = unsigned(options.cursor); bucket_id < 256; ++bucket_id) {
            const auto group = bucket_id & (root_.groups() - 1);
            if (!root_.sizes[group]) continue;
            auto visitor = [&](const std::string& encoded) {
                const packed_hash::page_view page(encoded,root_,group);
                for (unsigned i = 0; i < page.count(); ++i) {
                    const auto item = page.at(i);
                    if (scan_bucket(item.first.data(),item.first.size()) != bucket_id) continue;
                    ++visited;
                    auto field = item.first.str();
                    if (!scan_accept(field,options)) continue;
                    result.values.push_back(std::move(field));
                    if (!options.no_values) result.values.push_back(item.second.str());
                }
            };
            if (!inspect(group,visitor)) return false;
            if (visited >= options.count) {
                result.cursor = bucket_id == 255 ? 0 : bucket_id + 1;
                out = std::move(result); return true;
            }
        }
    } catch (const std::runtime_error&) { return fail(status::invalid); }
    out = std::move(result); return true;
}
bool packed_hash_store::set_expiry(int64_t deadline) {
    if (error_ != status::ok) return false;
    if (deadline < -1) return fail(status::invalid);
    if (native_) return with_native([&](map_store& store) { return store.set_expiry(deadline); });
    if (!elements_ || expiry() == deadline) return true;
    auto next = root_; next.expires_at_ms = deadline; return publish(next);
}
bool packed_hash_store::clear() {
    if (error_ != status::ok) return false;
    if (!tx_.reset_pages(owner_)) return fail(status::storage);
    entry_->clear_pages = false; entry_->action = db::update_action::erase;
    entry_->maintenance_only = false;
    root_ = packed_hash::root(); elements_ = 0; native_ = false; cleared_ = true; return true;
}
}
