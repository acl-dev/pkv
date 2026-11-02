#include "stdafx.h"
#include "db.h"
#include "page_transaction.h"
#include "coder/record_codec.h"

namespace pkv {

db::batch_status db::watch(std::vector<std::string> keys,
    std::vector<watch_token>& out) {
    std::vector<watch_token> pending;
    const auto status = transact_page_reads(std::move(keys),
        [&](page_transaction& tx) {
            // Only actor-local views are lazy; shared databases initialize
            // their registry before serving any concurrent commands.
            if (!watch_registry_)
                watch_registry_ = std::make_shared<watch_registry>();
            const auto now = unix_time_ms();
            pending.reserve(tx.roots().size());
            for (const auto& root : tx.roots()) {
                bool existed = root.found;
                long long deadline = -1;
                if (existed) {
                    record_header header;
                    if (codec_->stored_header(root.value, header) !=
                        codec_status::ok) return false;
                    deadline = header.expires_at_ms;
                    existed = deadline < 0 || deadline > now;
                    if (!existed) deadline = -1;
                }
                pending.push_back(watch_registry_->subscribe(root.key,
                    existed, deadline, now));
            }
            return true;
        });
    if (status == batch_status::ok) out.swap(pending);
    return status;
}

} // namespace pkv
