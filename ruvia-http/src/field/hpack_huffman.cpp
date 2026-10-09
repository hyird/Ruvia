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
        std::size_t node_value = 0;
        const auto code = hpack_huffman_codes[symbol];
        const auto length = hpack_huffman_lengths[symbol];
        if (length < 5) {
            throw "Four-bit transitions require Huffman codes of at least five bits";
        }
        for (std::uint8_t remaining = length; remaining != 0; --remaining) {
            if (tree.nodes_[node_value].symbol_ >= 0) {
                throw "HPACK Huffman code has an existing symbol as a prefix";
            }
            const auto bit = (code >> (remaining - 1)) & 1U;
            auto& next_value = tree.nodes_[node_value].child_[bit];
            if (next_value < 0) {
                if (size == node_count) {
                    throw "HPACK Huffman tree exceeds the complete-tree node count";
                }
                next_value = static_cast<std::int16_t>(size++);
            }
            node_value = static_cast<std::size_t>(next_value);
        }
        auto& leaf = tree.nodes_[node_value];
        if (leaf.symbol_ >= 0 || leaf.child_[0] >= 0 || leaf.child_[1] >= 0) {
            throw "HPACK Huffman code duplicates or prefixes another symbol";
        }
        leaf.symbol_ = static_cast<std::int16_t>(symbol);
    }
    if (size != node_count) {
        throw "HPACK Huffman tree is incomplete";
    }
    std::size_t state_value = 0;
    for (std::size_t node_value = 0; node_value < node_count; ++node_value) {
        auto& entry_value = tree.nodes_[node_value];
        if (entry_value.symbol_ < 0) {
            if (state_value == state_count || entry_value.child_[0] < 0 || entry_value.child_[1] < 0) {
                throw "HPACK Huffman tree has invalid internal nodes";
            }
            entry_value.state_ = static_cast<std::uint8_t>(state_value);
            tree.states_[state_value++] = node_value;
        }
    }
    if (state_value != state_count) {
        throw "HPACK Huffman tree has an unexpected state count";
    }
    // RFC 7541 section 5.2: only zero to seven leading bits of EOS may remain.
    if (hpack_huffman_codes.back() != 0x3fffffffU || hpack_huffman_lengths.back() != 30) {
        throw "HPACK Huffman EOS must consist of thirty one bits";
    }
    std::size_t node_value = 0;
    for (std::size_t depth = 0; depth <= 7; ++depth) {
        tree.nodes_[node_value].accepting_ = true;
        node_value = static_cast<std::size_t>(tree.nodes_[node_value].child_[1]);
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
    std::array<std::array<huffman_step, 16>, state_count> result_value{};
    for (std::size_t state_value = 0; state_value < state_count; ++state_value) {
        for (std::size_t nibble = 0; nibble < 16; ++nibble) {
            auto node_value = tree.states_[state_value];
            auto& step = result_value[state_value][nibble];
            for (int shift = 3; shift >= 0; --shift) {
                node_value = static_cast<std::size_t>(tree.nodes_[node_value].child_[(nibble >> shift) & 1U]);
                const auto symbol = tree.nodes_[node_value].symbol_;
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
                    node_value = 0;
                }
            }
            if ((step.flags_ & invalid) == 0) {
                step.state_ = tree.nodes_[node_value].state_;
                if (tree.nodes_[node_value].accepting_) {
                    step.flags_ |= accepting;
                }
            }
        }
    }
    return result_value;
}

constexpr auto transitions = build_transitions();

}  // namespace

bool append_hpack_huffman(std::string_view encoded, std::pmr::string& output) {
    std::uint8_t state_value = 0;
    std::uint8_t flags = accepting;
    const auto advance = [&](std::uint8_t nibble) {
        const auto step = transitions[state_value][nibble];
        state_value = step.state_;
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
