#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/borrowed_text.h"

namespace ruvia::detail {

// Concatenates pieces into inline storage when they fit, otherwise into one
// string sized up front. view() borrows this object, so it neither copies nor moves.
template <std::size_t inline_capacity>
class inline_text_concat final {
public:
    inline_text_concat(std::initializer_list<std::string_view> pieces) {
        for (const auto piece : pieces) {
            if (piece.size() > (std::numeric_limits<std::size_t>::max)() - size_) {
                throw std::length_error("concatenated text length overflow");
            }
            size_ += piece.size();
        }
        if (size_ <= inline_capacity) {
            auto* output = inline_.data();
            for (const auto piece : pieces) {
                output = std::ranges::copy(piece, output).out;
            }
            return;
        }
        heap_.reserve(size_);
        for (const auto piece : pieces) {
            heap_.append(piece);
        }
    }

    inline_text_concat(const inline_text_concat&) = delete;
    inline_text_concat& operator=(const inline_text_concat&) = delete;

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return size_ <= inline_capacity ? std::string_view(inline_.data(), size_) : std::string_view(heap_);
    }
    std::string_view view() const&& = delete;

private:
    std::array<char, inline_capacity> inline_;
    std::string heap_;
    std::size_t size_{0};
};

// Append-only text with inline storage; spilling preserves the caller's resource.
template <std::size_t inline_capacity>
class inline_text_buffer final {
public:
    explicit inline_text_buffer(std::size_t reserve_hint = 0)
        : reserve_hint_(reserve_hint) {}

    inline_text_buffer(const inline_text_buffer&) = delete;
    inline_text_buffer& operator=(const inline_text_buffer&) = delete;

    void push_back(char value) {
        append(&value, 1);
    }

    void append(const char* data, std::size_t count) {
        if (count == 0) {
            return;
        }
        const auto current_size = size();
        if (count > heap_.max_size() - current_size) {
            throw std::length_error("appended text length overflow");
        }
        if (heap_.empty() && count <= inline_capacity - inline_size_) {
            std::ranges::copy(std::string_view(data, count), inline_.data() + inline_size_);
            inline_size_ += count;
            return;
        }
        if (heap_.empty()) {
            const auto spill_capacity = inline_capacity <= heap_.max_size() / 2 ? inline_capacity * 2 : heap_.max_size();
            heap_.reserve((std::max)({reserve_hint_, current_size + count, spill_capacity}));
            heap_.assign(inline_.data(), inline_size_);
        }
        heap_.append(data, count);
    }

    void append(std::string_view text) {
        append(text.data(), text.size());
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return heap_.empty() ? inline_size_ : heap_.size();
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return heap_.empty() ? std::string_view(inline_.data(), inline_size_) : std::string_view(heap_);
    }
    std::string_view view() const&& = delete;

private:
    std::array<char, inline_capacity> inline_;
    std::string heap_;
    std::size_t inline_size_{0};
    std::size_t reserve_hint_;
};

}  // namespace ruvia::detail
