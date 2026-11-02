#include "exec_batch.h"
#include "db/database_pool.h"
#include "db/replica_group.h"
#include "db/family_workspace.h"

namespace pkv {
db::batch_status exec_batch::apply(database_pool& destinations) const {
    if (has_moves()) {
        try {
            family_workspace view(destinations);
            for (const auto& entry : entries_) {
                const auto status = entry.pages() ? view.stage(entry.pages()) :
                    view.stage(*entry.move());
                if (status != db::batch_status::ok) return status;
            }
            return view.commit();
        } catch (...) { return db::batch_status::error; }
    }
    unsigned selected;
    if (database(selected)) {
        const auto target = destinations.get(selected);
        return target ? apply(*target) : db::batch_status::error;
    }
    return replica_group::apply(destinations, *this);
}
} // namespace pkv
