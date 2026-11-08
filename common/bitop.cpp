#include "bitop.h"
#include <cstdint>
#include <cstring>

namespace pkv {
namespace {
template<bool UsesState, class Operation>
void combine(char* output, char* state, const char* input, size_t size,
    Operation operation) {
    size_t offset = 0;
    for (; size - offset >= sizeof(uint64_t); offset += sizeof(uint64_t)) {
        uint64_t value, mask, next;
        std::memcpy(&value, output + offset, sizeof(value));
        mask = 0;
        if (UsesState) std::memcpy(&mask, state + offset, sizeof(mask));
        std::memcpy(&next, input + offset, sizeof(next));
        value = operation(value, next, mask);
        std::memcpy(output + offset, &value, sizeof(value));
        if (UsesState) std::memcpy(state + offset, &mask, sizeof(mask));
    }
    for (; offset < size; ++offset) {
        uint64_t mask = UsesState ? static_cast<unsigned char>(state[offset]) : 0;
        output[offset] = static_cast<char>(operation(
            static_cast<unsigned char>(output[offset]),
            static_cast<unsigned char>(input[offset]), mask));
        if (UsesState) state[offset] = static_cast<char>(mask);
    }
}
} // namespace

bool valid_bitop(bitmap_operation operation, size_t sources) {
    if (!sources) return false;
    switch (operation) {
    case bitmap_operation::invert: return sources == 1;
    case bitmap_operation::difference:
    case bitmap_operation::reverse_difference:
    case bitmap_operation::and_or: return sources >= 2;
    case bitmap_operation::conjunction:
    case bitmap_operation::disjunction:
    case bitmap_operation::exclusive_or:
    case bitmap_operation::one: return true;
    }
    return false;
}

void bitop_fold(bitmap_operation operation, char* output, char* state,
    const char* input, size_t size) {
    switch (operation) {
    case bitmap_operation::conjunction:
        combine<false>(output, state, input, size,
            [](uint64_t a, uint64_t b, uint64_t&) { return a & b; }); break;
    case bitmap_operation::disjunction:
        combine<false>(output, state, input, size,
            [](uint64_t a, uint64_t b, uint64_t&) { return a | b; }); break;
    case bitmap_operation::exclusive_or:
        combine<false>(output, state, input, size,
            [](uint64_t a, uint64_t b, uint64_t&) { return a ^ b; }); break;
    case bitmap_operation::difference:
    case bitmap_operation::reverse_difference:
    case bitmap_operation::and_or:
        combine<true>(output, state, input, size,
            [](uint64_t a, uint64_t b, uint64_t& mask) {
                mask |= b; return a;
            }); break;
    case bitmap_operation::one:
        combine<true>(output, state, input, size,
            [](uint64_t a, uint64_t b, uint64_t& mask) {
                mask |= a & b; return (a ^ b) & ~mask;
            }); break;
    case bitmap_operation::invert: break;
    }
}

void bitop_finish(bitmap_operation operation, char* output, char* state,
    size_t size) {
    switch (operation) {
    case bitmap_operation::invert:
        combine<false>(output, state, output, size,
            [](uint64_t a, uint64_t, uint64_t&) { return ~a; }); break;
    case bitmap_operation::difference:
        combine<true>(output, state, state, size,
            [](uint64_t a, uint64_t b, uint64_t&) { return a & ~b; }); break;
    case bitmap_operation::reverse_difference:
        combine<true>(output, state, state, size,
            [](uint64_t a, uint64_t b, uint64_t&) { return ~a & b; }); break;
    case bitmap_operation::and_or:
        combine<true>(output, state, state, size,
            [](uint64_t a, uint64_t b, uint64_t&) { return a & b; }); break;
    default: break;
    }
}

} // namespace pkv
