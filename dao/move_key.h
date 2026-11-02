#pragma once
#include "dao_base.h"
#include "db/move_memory.h"

namespace pkv {
class database_pool;
class move_journal;
class family_publication;
namespace dao {
class move_key : public dao_base {
public:
    bool run(database_pool& pool, move_journal* journal, size_t source,
        size_t target, const std::string& key, bool& moved);
    const char* error() const {
        return journal_required_ ? "ERR MOVE requires an enabled native "
            "journal" : dao_base::error();
    }
private:
    bool journal_required_ = false;
    bool inspect(shared_db& database, const std::string& key, bool& alive);
    bool normalize(shared_db& database, const std::string& key, bool& alive);
    bool checked(db::batch_status status);
    move_memory::choice prepare(shared_db& source, shared_db& target,
        size_t from, size_t to, const move_memory::roots& roots,
        std::unique_ptr<family_publication>& publication,
        std::unique_ptr<family_publication>& effects, bool memory);
};
} // namespace dao
} // namespace pkv
