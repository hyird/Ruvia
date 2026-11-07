#pragma once

#ifdef RUVIA_ENABLE_JWT

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/auth/jwt.h"

// Shared between jwt.cpp (token signing and verification) and jwt_json.cpp
// (JOSE header and claims JSON).
namespace ruvia::detail {

class jwt_json_buffer final {
public:
    static constexpr std::size_t inline_capacity = 512;

    explicit jwt_json_buffer(std::pmr::memory_resource* resource, std::size_t reserve_hint = 0)
        : heap_(pmr_resource_or_default(resource)),
          reserve_hint_(reserve_hint) {
        if (reserve_hint_ > inline_capacity) {
            heap_.reserve(reserve_hint_);
        }
    }

    void push_back(char ch) {
        if (heap_.empty() && size_ < inline_capacity) {
            inline_[size_++] = ch;
            return;
        }
        spill_if_needed(1);
        heap_.push_back(ch);
    }

    void append(const char* data, std::size_t count) {
        if (count == 0) {
            return;
        }
        if (heap_.empty() && count <= inline_capacity - size_) {
            std::memcpy(inline_.data() + size_, data, count);
            size_ += count;
            return;
        }
        spill_if_needed(count);
        heap_.append(data, count);
    }

    void append(std::string_view text) {
        append(text.data(), text.size());
    }

    [[nodiscard]] std::string_view view() const& noexcept RUVIA_LIFETIMEBOUND {
        return heap_.empty() ? std::string_view(inline_.data(), size_) : std::string_view(heap_);
    }
    std::string_view view() const&& = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return heap_.empty() ? size_ : heap_.size();
    }

private:
    void spill_if_needed(std::size_t additional) {
        if (!heap_.empty()) {
            return;
        }
        heap_.reserve((std::max)(reserve_hint_, size_ + additional));
        heap_.assign(inline_.data(), size_);
    }

    std::array<char, inline_capacity> inline_{};
    std::pmr::string heap_;
    std::size_t size_{0};
    std::size_t reserve_hint_{0};
};

[[nodiscard]] constexpr std::uint8_t jwt_reserved_claim_bit(std::string_view name) noexcept {
    if (name.size() != 3) {
        return 0;
    }
    if (name == "iss") {
        return 1U << 0;
    }
    if (name == "sub") {
        return 1U << 1;
    }
    if (name == "aud") {
        return 1U << 2;
    }
    if (name == "jti") {
        return 1U << 3;
    }
    if (name == "exp") {
        return 1U << 4;
    }
    if (name == "nbf") {
        return 1U << 5;
    }
    if (name == "iat") {
        return 1U << 6;
    }
    return 0;
}

[[nodiscard]] constexpr bool jwt_is_reserved_claim(std::string_view name) noexcept {
    return jwt_reserved_claim_bit(name) != 0;
}

void jwt_append_json_member(
    jwt_json_buffer& out, bool& first, std::string_view name, std::string_view value);
void jwt_append_json_numeric_date(
    jwt_json_buffer& out, bool& first, std::string_view name, std::string_view value);
void jwt_validate_jose_header(std::string_view json,
    std::optional<std::string_view> expected_algorithm, std::pmr::memory_resource* resource);

struct jwt_payload_access final {
    [[nodiscard]] static jwt_claim claim(std::pmr::string name, std::pmr::string value) noexcept {
        return jwt_claim(std::move(name), std::move(value));
    }
    [[nodiscard]] static jwt_numeric_date numeric_date(
        std::string_view text, std::pmr::memory_resource* resource) {
        return jwt_numeric_date(text, resource);
    }

    [[nodiscard]] static bool decode_jwt_payload_claim(jwt_payload& payload_value, std::string_view key,
        std::string_view& value, std::uint8_t& seen_reserved_mask, std::pmr::memory_resource* resource);
    static jwt_payload decode_payload_json(std::string_view json, std::pmr::memory_resource* resource);
};

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_JWT
