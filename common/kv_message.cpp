#include "kv_message.h"
#include "rpc/page_batch.h"
#include "rpc/exec_batch.h"

namespace pkv {

kv_message::kv_message(std::shared_ptr<const page_batch> pages)
: oper_(message_oper_pages)
, database_(pages ? pages->database() : 0)
, protocol_(2)
, pages_(std::move(pages))
, refers_(1)
{}

kv_message::kv_message(std::shared_ptr<const exec_batch> group)
: oper_(message_oper_exec), database_(0)
, protocol_(group ? group->protocol() : 3)
, exec_(std::move(group))
, refers_(1) {}

size_t kv_message::wire_bytes() const {
    if (exec_) return exec_->bytes();
    return pages_ ? pages_->size().bytes : 20 + key_.size() + value_.size();
}

size_t kv_message::wire_records() const {
    if (exec_) return exec_->records();
    return pages_ ? pages_->size().records : 1;
}

} // namespace pkv
