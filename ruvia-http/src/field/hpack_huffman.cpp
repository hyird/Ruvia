#include "field/hpack_huffman.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "field/hpack_huffman_tables.h"

namespace ruvia::detail {
namespace {

constexpr std::size_t symbol_count = hpack_huffman_codes.size();
constexpr std::size_t state_count = symbol_count - 1;
constexpr std::size_t node_count = symbol_count * 2 - 1;
static_assert(state_count == 256);
static_assert(symbol_count == hpack_huffman_lengths.size());

struct huffman_node final {
    std::array<std::int16_t, 2> child_{-1, -1};
    std::int16_t symbol_{-1};
    std::uint8_t state_{0};
    bool accepting_{false};
};

struct huffman_tree final {
    std::array<huffman_node, node_count> nodes_{};
    std::array<std::size_t, state_count> states_{};
};

consteval huffman_tree build_tree() {
    huffman_tree tree;
    std::size_t size = 1;
    for (std::size_t symbol = 0; symbol < symbol_count; ++symbol) {
        std::size_t node = 0;
        const auto code = hpack_huffman_codes[symbol];
        const auto length = hpack_huffman_lengths[symbol];
        if (length < 5) {
            throw "Four-bit transitions require Huffman codes of at least five bits";
        }
        for (std::uint8_t remaining = length; remaining != 0; --remaining) {
            if (tree.nodes_[node].symbol_ >= 0) {
                throw "HPACK Huffman code has an existing symbol as a prefix";
            }
            const auto bit = (code >> (remaining - 1)) & 1U;
            auto& next = tree.nodes_[node].child_[bit];
            if (next < 0) {
                if (size == node_count) {
                    throw "HPACK Huffman tree exceeds the complete-tree node count";
                }
                next = static_cast<std::int16_t>(size++);
            }
            node = static_cast<std::size_t>(next);
        }
        auto& leaf = tree.nodes_[node];
        if (leaf.symbol_ >= 0 || leaf.child_[0] >= 0 || leaf.child_[1] >= 0) {
            throw "HPACK Huffman code duplicates or prefixes another symbol";
        }
        leaf.symbol_ = static_cast<std::int16_t>(symbol);
    }
    if (size != node_count) {
        throw "HPACK Huffman tree is incomplete";
    }
    std::size_t state = 0;
    for (std::size_t node = 0; node < node_count; ++node) {
        auto& entry = tree.nodes_[node];
        if (entry.symbol_ < 0) {
            if (state == state_count || entry.child_[0] < 0 || entry.child_[1] < 0) {
                throw "HPACK Huffman tree has invalid internal nodes";
            }
            entry.state_ = static_cast<std::uint8_t>(state);
            tree.states_[state++] = node;
        }
    }
    if (state != state_count) {
        throw "HPACK Huffman tree has an unexpected state count";
    }
    // RFC 7541 section 5.2: only zero to seven leading bits of EOS may remain.
    if (hpack_huffman_codes.back() != 0x3fffffffU || hpack_huffman_lengths.back() != 30) {
        throw "HPACK Huffman EOS must consist of thirty one bits";
    }
    std::size_t node = 0;
    for (std::size_t depth = 0; depth <= 7; ++depth) {
        tree.nodes_[node].accepting_ = true;
        node = static_cast<std::size_t>(tree.nodes_[node].child_[1]);
    }
    return tree;
}

constexpr auto tree = build_tree();
constexpr std::uint8_t accepting = 1;
constexpr std::uint8_t emit = 2;
constexpr std::uint8_t invalid = 4;

struct huffman_step final {
    std::uint8_t state_{0};
    std::uint8_t symbol_{0};
    std::uint8_t flags_{0};
};

consteval auto build_transitions() {
    std::array<std::array<huffman_step, 16>, state_count> result{};
    for (std::size_t state = 0; state < state_count; ++state) {
        for (std::size_t nibble = 0; nibble < 16; ++nibble) {
            auto node = tree.states_[state];
            auto& step = result[state][nibble];
            for (int shift = 3; shift >= 0; --shift) {
                node = static_cast<std::size_t>(tree.nodes_[node].child_[(nibble >> shift) & 1U]);
                const auto symbol = tree.nodes_[node].symbol_;
                if (symbol == 256) {
                    step.flags_ = invalid;
                    break;
                }
                if (symbol >= 0) {
                    if ((step.flags_ & emit) != 0) {
                        throw "A four-bit Huffman transition cannot emit two symbols";
                    }
                    step.symbol_ = static_cast<std::uint8_t>(symbol);
                    step.flags_ = emit;
                    node = 0;
                }
            }
            if ((step.flags_ & invalid) == 0) {
                step.state_ = tree.nodes_[node].state_;
                if (tree.nodes_[node].accepting_) {
                    step.flags_ |= accepting;
                }
            }
        }
    }
    return result;
}

constexpr auto transitions = build_transitions();

}  // namespace

bool append_hpack_huffman(std::string_view encoded, std::pmr::string& output) {
    std::uint8_t state = 0;
    std::uint8_t flags = accepting;
    const auto advance = [&](std::uint8_t nibble) {
        const auto step = transitions[state][nibble];
        state = step.state_;
        flags = step.flags_;
        if ((flags & emit) != 0) {
            output.push_back(static_cast<char>(step.symbol_));
        } else if ((flags & invalid) != 0) {
            return false;
        }
        return true;
    };
    for (const auto byte_value : encoded) {
        const auto byte = static_cast<std::uint8_t>(byte_value);
        if (!advance(byte >> 4) || !advance(byte & 0xfU)) {
            return false;
        }
    }
    return (flags & accepting) != 0;
}

}  // namespace ruvia::detail
