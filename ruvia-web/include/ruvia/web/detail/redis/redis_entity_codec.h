#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "ruvia/http/http_number_format.h"
#include "ruvia/web/detail/redis/redis_entity_traits.h"
#include "ruvia/web/redis/redis_types.h"

namespace ruvia::detail {

[[noreturn]] inline void throw_invalid_redis_scalar(std::string_view message) {
    throw redis_error(redis_error::code_type::protocol_error, message);
}

inline constexpr std::size_t redis_scalar_scratch_size = 128;
using redis_scalar_scratch_type = std::array<char, redis_scalar_scratch_size>;

template <typename t_type>
[[nodiscard]] std::string_view encode_redis_number(
    t_type value, std::span<char, redis_scalar_scratch_size> scratch) {
    if constexpr (std::is_floating_point_v<t_type>) {
        if (!std::isfinite(value)) {
            throw_invalid_redis_scalar("non-finite Redis scalar");
        }
        const auto result_value = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value,
            std::chars_format::general, std::numeric_limits<t_type>::max_digits10);
        if (result_value.ec != std::errc{}) {
            throw_invalid_redis_scalar("failed to encode Redis scalar");
        }
        return {scratch.data(), static_cast<std::size_t>(result_value.ptr - scratch.data())};
    } else if constexpr (std::is_unsigned_v<t_type>) {
        return http_format_unsigned(
            std::span<char, http_max_unsigned_decimal_size>(scratch.data(), http_max_unsigned_decimal_size), value);
    } else {
        const auto [ptr, ec] = std::to_chars(scratch.data(), scratch.data() + scratch.size(), value);
        if (ec != std::errc{}) {
            throw std::logic_error("failed to encode Redis scalar");
        }
        return {scratch.data(), static_cast<std::size_t>(ptr - scratch.data())};
    }
}

template <typename t_type>
[[nodiscard]] t_type decode_redis_number(std::string_view value) {
    t_type result_value{};
    const auto parsed_value = [&] {
        if constexpr (std::is_floating_point_v<t_type>) {
            return std::from_chars(value.data(), value.data() + value.size(), result_value,
                std::chars_format::general);
        } else {
            return std::from_chars(value.data(), value.data() + value.size(), result_value);
        }
    }();
    if (parsed_value.ec != std::errc{} || parsed_value.ptr != value.data() + value.size() ||
        (std::is_floating_point_v<t_type> && !std::isfinite(result_value))) {
        throw_invalid_redis_scalar("malformed Redis scalar");
    }
    return result_value;
}

template <typename t_type>
[[nodiscard]] std::string_view encode_redis_scalar(
    const t_type& value, std::span<char, redis_scalar_scratch_size> scratch) {
    using scalar_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<scalar_type, string>) {
        return value.view();
    } else if constexpr (std::is_same_v<scalar_type, std::pmr::string> ||
                         std::is_same_v<scalar_type, std::string> ||
                         std::is_same_v<scalar_type, std::string_view>) {
        return value;
    } else if constexpr (std::is_same_v<scalar_type, bool>) {
        return value ? std::string_view("1") : std::string_view("0");
    } else if constexpr (is_redis_entity_native_number<scalar_type>) {
        return encode_redis_number(value, scratch);
    } else {
        static_assert(always_false<scalar_type>, "unsupported Redis scalar type");
    }
}

template <typename t_type>
[[nodiscard]] t_type decode_redis_scalar(
    std::string_view value, std::pmr::memory_resource* resource) {
    auto* const memory_resource = pmr_resource_or_default(resource);
    using scalar_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<scalar_type, string>) {
        string result_value(model_options{.resource_ = memory_resource});
        result_value.assign_owned(value);
        return result_value;
    } else if constexpr (std::is_same_v<scalar_type, std::pmr::string>) {
        return std::pmr::string(value.data(), value.size(), memory_resource);
    } else if constexpr (std::is_same_v<scalar_type, bool>) {
        if (value == "0") {
            return false;
        }
        if (value == "1") {
            return true;
        }
        throw_invalid_redis_scalar("malformed Redis boolean scalar");
    } else if constexpr (is_redis_entity_native_number<scalar_type>) {
        return decode_redis_number<scalar_type>(value);
    } else {
        static_assert(always_false<scalar_type>, "unsupported Redis scalar type");
    }
}

}  // namespace ruvia::detail
