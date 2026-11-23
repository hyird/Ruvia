#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "http_io_buffer.h"
#include "http_message.h"
#include "inbound_buffer.h"

namespace ruvia::detail {

[[nodiscard]] inline std::size_t inbound_string_heap_bytes(const std::string& value) noexcept {
    static const std::size_t sso_capacity = std::string{}.capacity();
    return value.capacity() > sso_capacity ? value.capacity() : 0;
}

inline void inbound_reserve(std::string& target, inbound_buffer_reservation& reservation, std::size_t capacity) {
    if (capacity <= target.capacity()) {
        return;
    }
    static const std::size_t sso_capacity = std::string{}.capacity();
    if (capacity > sso_capacity) {
        reservation.resize_to(capacity);
    }
    try {
        target.reserve(capacity);
    } catch (...) {
        reservation.shrink_to(inbound_string_heap_bytes(target));
        throw;
    }
    reservation.resize_to(inbound_string_heap_bytes(target));
}

inline void inbound_resize(std::string& target, inbound_buffer_reservation& reservation, std::size_t new_size) {
    if (new_size > target.capacity()) {
        static const std::size_t sso_capacity = std::string{}.capacity();
        if (new_size > sso_capacity) {
            reservation.resize_to(new_size);
        }
        try {
            target.resize(new_size);
        } catch (...) {
            reservation.shrink_to(inbound_string_heap_bytes(target));
            throw;
        }
        reservation.resize_to(inbound_string_heap_bytes(target));
    } else {
        target.resize(new_size);
        reservation.shrink_to(inbound_string_heap_bytes(target));
    }
}

inline void inbound_append(std::string& target, inbound_buffer_reservation& reservation, std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    if (bytes.size() > target.max_size() - target.size()) {
        throw std::length_error("inbound buffer length overflow");
    }
    const auto needed = target.size() + bytes.size();
    if (needed > target.capacity()) {
        const auto doubled = target.capacity() > target.max_size() / 2 ? target.max_size() : target.capacity() * 2;
        inbound_reserve(target, reservation, (std::max)(needed, doubled));
    }
    target.append(bytes.data(), bytes.size());
    reservation.resize_to(inbound_string_heap_bytes(target));
}

inline void inbound_clear_retaining_small(std::string& target, inbound_buffer_reservation& reservation, std::size_t retained_bytes = http_retained_string_bytes) noexcept {
    http_clear_string_retaining_small(target, retained_bytes);
    reservation.shrink_to(inbound_string_heap_bytes(target));
}

inline void inbound_discard(std::string& target, inbound_buffer_reservation& reservation) noexcept {
    std::string{}.swap(target);
    reservation.clear();
}

[[nodiscard]] inline std::span<char> inbound_prepare(http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::size_t bytes) {
    const auto spare = buffer.prepare_spare();
    if (spare.size() >= bytes) {
        return spare.first(bytes);
    }
    constexpr std::size_t max_bytes = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
    if (bytes > max_bytes - buffer.size()) {
        throw std::length_error("http_io_buffer size exceeds the addressable range");
    }
    const auto doubled = buffer.capacity() > max_bytes / 2 ? max_bytes : buffer.capacity() * 2;
    const auto next_capacity = (std::max)(buffer.size() + bytes, doubled);
    if (next_capacity > buffer.capacity()) {
        reservation.resize_to(next_capacity);
    }
    try {
        const auto prepared = buffer.prepare(bytes);
        reservation.resize_to(buffer.capacity());
        return prepared;
    } catch (...) {
        reservation.shrink_to(buffer.capacity());
        throw;
    }
}

inline void inbound_reserve(http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::size_t capacity) {
    if (capacity <= buffer.capacity()) {
        return;
    }
    reservation.resize_to(capacity);
    try {
        buffer.reserve(capacity);
    } catch (...) {
        reservation.shrink_to(buffer.capacity());
        throw;
    }
    reservation.resize_to(buffer.capacity());
}

inline void inbound_append(http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::string_view bytes) {
    if (bytes.empty()) {
        return;
    }
    const auto spare = buffer.prepare_spare();
    if (spare.size() < bytes.size()) {
        constexpr std::size_t max_bytes = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
        if (bytes.size() > max_bytes - buffer.size()) {
            throw std::length_error("http_io_buffer size exceeds the addressable range");
        }
        const auto doubled = buffer.capacity() > max_bytes / 2 ? max_bytes : buffer.capacity() * 2;
        const auto next_capacity = (std::max)(buffer.size() + bytes.size(), doubled);
        if (next_capacity > buffer.capacity()) {
            reservation.resize_to(next_capacity);
        }
    }
    try {
        buffer.append(bytes);
        reservation.resize_to(buffer.capacity());
    } catch (...) {
        reservation.shrink_to(buffer.capacity());
        throw;
    }
}

inline void inbound_shrink_to(http_io_buffer& buffer, inbound_buffer_reservation& reservation, std::size_t bytes) {
    buffer.shrink_to(bytes);
    reservation.shrink_to(buffer.capacity());
}

}  // namespace ruvia::detail
