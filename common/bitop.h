#pragma once
#include <cstddef>

namespace pkv {

enum class bitmap_operation {
    conjunction, disjunction, exclusive_or, invert, difference,
    reverse_difference, and_or, one
};
bool valid_bitop(bitmap_operation operation, size_t sources);
inline bool bitop_uses_state(bitmap_operation operation) {
    return operation == bitmap_operation::difference ||
        operation == bitmap_operation::reverse_difference ||
        operation == bitmap_operation::and_or ||
        operation == bitmap_operation::one;
}
// Buffers have exactly size bytes. When used, state starts at zero for each
// chunk. Otherwise state may be null.
// output starts with the first zero-padded input. Fold remaining inputs, then
// finalize. Input multiplicity matters for XOR and ONE.
void bitop_fold(bitmap_operation operation, char* output, char* state,
    const char* input, size_t size);
void bitop_finish(bitmap_operation operation, char* output, char* state,
    size_t size);

} // namespace pkv
