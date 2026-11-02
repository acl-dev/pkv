#include "db.h"
#include "db_watcher.h"
#include <new>

namespace pkv {
namespace {
class move_observers : public family_publication {
public:
    std::vector<std::unique_ptr<family_publication>> entries;
    void publish() noexcept override {
        for (auto& entry : entries) if (entry) entry->publish();
        entries.clear();
    }
};
} // namespace
bool db_watchers::prepare_move(db_watchers* source, db_watchers* target,
    const family_move& move, std::unique_ptr<family_publication>& out) {
    out.reset();
    if ((source && source->database_ != move.source) ||
        (target && target->database_ != move.target)) return false;
    try {
        std::vector<db_watcher*> observers;
        for (auto* group : {source, target}) if (group) {
            std::lock_guard<std::mutex> guard(group->lock_);
            for (auto* observer : group->watchers_)
                if (std::find(observers.begin(), observers.end(), observer)
                    == observers.end()) observers.push_back(observer);
        }
        if (observers.empty()) return true;
        std::unique_ptr<move_observers> prepared(new move_observers);
        prepared->entries.reserve(observers.size());
        for (auto* observer : observers) {
            std::unique_ptr<family_publication> entry;
            if (!observer->prepare_move(move, entry)) return false;
            if (entry) prepared->entries.push_back(std::move(entry));
        }
        out = std::move(prepared); return true;
    } catch (const std::bad_alloc&) { return false; }
}
bool db::prepare_move_observers(db& target, const family_move& move,
    std::unique_ptr<family_publication>& out) {
    return db_watchers::prepare_move(watchers_, target.watchers_, move, out);
}
} // namespace pkv
