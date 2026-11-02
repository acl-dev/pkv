#pragma once
#include <cstddef>

namespace pkv {

enum class bitmap_operation {
    conjunction, disjunction, exclusive_or, invert, difference,
    reverse_difference, and_or, one
};
bool valid_bitop(bitmap_operation operation, size_t sources);
// Buffers have exactly size bytes. state starts at zero for each chunk;
// output starts with the first zero-padded input. Fold remaining inputs, then
// finalize. Input multiplicity matters for XOR and ONE.
void bitop_fold(bitmap_operation operation, char* output, char* state,
    const char* input, size_t size);
void bitop_finish(bitmap_operation operation, char* output, char* state,
    size_t size);

} // namespace pkv
