#include "redis_coder.h"
#include "common/client_memory.h"

namespace pkv {
void redis_object::memory(redis_memory& out) const {
    out.bytes += buf_.size();
    out.spare += buf_.capacity() - buf_.size();
    out.owned += sizeof(*this) + string_heap_bytes(buf_);
    if (objs_) {
        out.owned += sizeof(*objs_) +
            objs_->capacity() * sizeof(redis_object*);
        for (const auto* child : *objs_) child->memory(out);
    }
    if (obj_) obj_->memory(out);
}
redis_input_memory redis_coder::memory() const {
    redis_input_memory out;
    out.complete.owned += objs_.capacity() * sizeof(redis_object*);
    for (const auto* root : objs_) root->memory(out.complete);
    curr_->memory(out.partial);
    return out;
}
} // namespace pkv
