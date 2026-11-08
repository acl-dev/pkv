#pragma once
#include <cstdlib>
#include <cstddef>
#include <memory>
#include <vector>

namespace pkv {
// Nodes live in hash tables with stable addresses. Their mutable position is
// protected by the same lock as this array, independently of payload locks.
// A pending slot is private until publish(). Cancel/erase never allocate.
template<class Node> class dense_key_index {
public:
    using pointer = const Node*;
    size_t size() const { return size_; }
    pointer operator[](size_t position) const { return slot(position); }
    void prepare() {
        if (size_ < block_size) {
            first_.push_back(nullptr);
        } else {
            if (size_ % block_size == 0) {
                std::unique_ptr<pointer[]> block(new pointer[block_size]);
                blocks_.push_back(std::move(block));
            }
            slot(size_) = nullptr;
        }
        ++size_;
    }
    void publish(pointer node) {
        if (!size_ || slot(size_ - 1)) std::abort();
        node->second.position = size_ - 1;
        slot(size_ - 1) = node;
    }
    void cancel() {
        if (!size_ || slot(size_ - 1)) std::abort();
        pop();
    }
    void erase(pointer node) {
        const auto position = node->second.position;
        if (position >= size_ || slot(position) != node) std::abort();
        const auto moved = slot(size_ - 1);
        slot(position) = moved;
        moved->second.position = position;
        pop();
    }
private:
    static constexpr size_t block_size = 256;
    // Keep small indexes cheap. Beyond the first block, unused trailing
    // blocks can be released without allocating during commit or rollback.
    std::vector<pointer> first_;
    std::vector<std::unique_ptr<pointer[]>> blocks_;
    size_t size_ = 0;

    const pointer& slot(size_t position) const {
        if (position < block_size) return first_[position];
        return blocks_[position / block_size - 1][position % block_size];
    }
    pointer& slot(size_t position) {
        if (position < block_size) return first_[position];
        return blocks_[position / block_size - 1][position % block_size];
    }
    void pop() {
        --size_;
        if (size_ >= block_size) {
            if (size_ % block_size == 0) blocks_.pop_back();
        } else {
            first_.pop_back();
            if (!size_) {
                if (first_.capacity() > 8) std::vector<pointer>().swap(first_);
                if (blocks_.capacity() > 8) decltype(blocks_)().swap(blocks_);
            }
        }
    }
};
} // namespace pkv
