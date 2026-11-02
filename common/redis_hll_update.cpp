#include "redis_hll.h"

namespace pkv {
namespace hll {
namespace {
void coalesce(std::string& value, size_t offset) {
    for (size_t step = 0; step < 5 && offset < value.size(); ++step) {
        run first, second;
        if (!decode_run(value, offset, first)) return;
        if (first.value && decode_run(value, offset + first.bytes, second)
            && second.value == first.value &&
            first.length + second.length <= 4) {
            std::string replacement;
            encode_run(replacement, first.length + second.length, first.value);
            value.replace(offset, 2, replacement);
        } else offset += first.bytes;
    }
}
bool dense_set(std::string& value, size_t index, uint8_t frequency,
    bool& changed) {
    if (!dense(value)) return false;
    changed = get(value, index) < frequency;
    if (changed) put(value, index, frequency);
    return true;
}
} // namespace
bool set(std::string& value, size_t index, uint8_t frequency, bool& changed) {
    changed = false;
    if (index >= registers_size || frequency > 63 ||
        !header(value, value.size())) return false;
    if (!value[4] || frequency > 32)
        return dense_set(value, index, frequency, changed);
    size_t first = 0, previous = header_size;
    for (size_t offset = header_size; offset < value.size();) {
        run part;
        if (!decode_run(value, offset, part) ||
            part.length > registers_size - first) return false;
        if (index >= first + part.length) {
            previous = offset; first += part.length; offset += part.bytes;
            continue;
        }
        if (part.value >= frequency) return true;
        std::string replacement;
        encode_run(replacement, index - first, part.value);
        encode_run(replacement, 1, frequency);
        encode_run(replacement, first + part.length - index - 1, part.value);
        if (replacement.size() > part.bytes && value.size() +
            replacement.size() - part.bytes > sparse_limit)
            return dense_set(value, index, frequency, changed);
        value.replace(offset, part.bytes, replacement);
        coalesce(value, previous); changed = true; return true;
    }
    return false;
}
} // namespace hll
} // namespace pkv
