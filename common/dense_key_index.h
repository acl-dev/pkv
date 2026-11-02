#pragma once
#include <cstdlib>
#include <cstddef>
#include <vector>

namespace pkv {
// Nodes live in hash tables with stable addresses. Their mutable position is
// protected by the same lock as this array, independently of payload locks.
// A pending slot is private until publish(). Cancel/erase never allocate.
template<class Node> class dense_key_index {
public:
    using pointer = const Node*;
    using iterator = typename std::vector<pointer>::const_iterator;
    size_t size() const { return nodes_.size(); }
    pointer operator[](size_t position) const { return nodes_[position]; }
    iterator begin() const { return nodes_.begin(); }
    iterator end() const { return nodes_.end(); }
    void prepare() { nodes_.push_back(nullptr); }
    void publish(pointer node) {
        if (nodes_.empty() || nodes_.back()) std::abort();
        node->second.position = nodes_.size() - 1;
        nodes_.back() = node;
    }
    void cancel() {
        if (nodes_.empty() || nodes_.back()) std::abort();
        nodes_.pop_back(); release_empty();
    }
    void erase(pointer node) {
        const auto position = node->second.position;
        if (position >= nodes_.size() || nodes_[position] != node)
            std::abort();
        const auto moved = nodes_.back();
        nodes_[position] = moved;
        moved->second.position = position;
        nodes_.pop_back(); release_empty();
    }
private:
    std::vector<pointer> nodes_;
    void release_empty() {
        // Reuse a few slots for repeated empty/nonempty churn. Large empty
        // arrays are released without an allocation during commit/rollback.
        if (nodes_.empty() && nodes_.capacity() > 8)
            std::vector<pointer>().swap(nodes_);
    }
};
} // namespace pkv
