#pragma once
#include "dao/dao_base.h"
#include <unordered_set>

namespace pkv {
namespace dao {
class json_scan : public dao_base {
public:
    bool random_key(shared_db& db, std::string& out, bool& found);
    bool all_keys(shared_db& db, const std::string& pattern,
        std::unordered_set<std::string>& out);
    bool visit_all_keys(shared_db& db, const std::string& pattern,
        const std::function<void(const std::string&)>& emit);
    bool keys(shared_db& db, const scan_options& options, scan_page& out);
    bool open_collection(shared_db& db, const std::string& key, value_kind kind);
    bool collection(const scan_options& options, scan_page& out);
private:
    bool stored_header(const record_codec& codec, const std::string& bytes,
        record_header& out);
    bool read_header(shared_db& db, const std::string& key,
        record_header& out);
    shared_db database_;
    std::string key_;
    value_kind kind_ = value_kind::string;
    bool collection_paged(const scan_options& options, scan_page& out);
};
}
}
