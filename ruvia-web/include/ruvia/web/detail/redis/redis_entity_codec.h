#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ruvia/web/detail/redis/redis_entity_traits.h"
#include "ruvia/web/redis/redis_types.h"

namespace ruvia::detail {

[[noreturn]] inline void throw_invalid_redis_scalar(std::string_view message) {
    throw redis_error(redis_error::code_type::protocol_error, message);
}

template <typename t_type>
[[nodiscard]] std::pmr::string encode_redis_number(
    t_type value, std::pmr::memory_resource* resource) {
    char buffer[128]{};
    const auto result_value = [&] {
        if constexpr (std::is_floating_point_v<t_type>) {
            if (!std::isfinite(value)) {
                throw_invalid_redis_scalar("non-finite Redis scalar");
            }
            return std::to_chars(buffer, std::end(buffer), value, std::chars_format::general,
                std::numeric_limits<t_type>::max_digits10);
        } else {
            return std::to_chars(buffer, std::end(buffer), value);
        }
    }();
    if (result_value.ec != std::errc{}) {
        throw_invalid_redis_scalar("failed to encode Redis scalar");
    }
    return std::pmr::string(buffer, static_cast<std::size_t>(result_value.ptr - buffer), resource);
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
[[nodiscard]] std::pmr::string encode_redis_scalar(
    const t_type& value, std::pmr::memory_resource* resource = nullptr) {
    auto* const memory_resource = pmr_resource_or_default(resource);
    using scalar_type = std::remove_cvref_t<t_type>;
    if constexpr (std::is_same_v<scalar_type, string>) {
        const auto text = value.view();
        return std::pmr::string(text.data(), text.size(), memory_resource);
    } else if constexpr (std::is_same_v<scalar_type, std::pmr::string>) {
        return std::pmr::string(value.data(), value.size(), memory_resource);
    } else if constexpr (std::is_same_v<scalar_type, std::string> ||
                         std::is_same_v<scalar_type, std::string_view>) {
        return std::pmr::string(value.data(), value.size(), memory_resource);
    } else if constexpr (std::is_same_v<scalar_type, bool>) {
        return std::pmr::string(value ? "1" : "0", memory_resource);
    } else if constexpr (std::is_same_v<scalar_type, bool_value>) {
        return std::pmr::string(static_cast<bool>(value) ? "1" : "0", memory_resource);
    } else if constexpr (is_ruvia_scalar<scalar_type>) {
        using raw_type = model_scalar_value_t_type<scalar_type>;
        return encode_redis_number(static_cast<raw_type>(value), memory_resource);
    } else if constexpr (is_redis_entity_native_number<scalar_type>) {
        return encode_redis_number(value, memory_resource);
    } else {
        static_assert(always_false<scalar_type>, "unsupported Redis scalar type");
    }
}

template <typename t_type>
[[nodiscard]] t_type decode_redis_scalar(
    std::string_view value, std::pmr::memory_resource* resource = nullptr) {
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
    } else if constexpr (std::is_same_v<scalar_type, bool_value>) {
        if (value == "0") {
            return bool_value{false};
        }
        if (value == "1") {
            return bool_value{true};
        }
        throw_invalid_redis_scalar("malformed Redis boolean scalar");
    } else if constexpr (is_ruvia_scalar<scalar_type>) {
        using raw_type = model_scalar_value_t_type<scalar_type>;
        return scalar_type(decode_redis_number<raw_type>(value));
    } else if constexpr (is_redis_entity_native_number<scalar_type>) {
        return decode_redis_number<scalar_type>(value);
    } else {
        static_assert(always_false<scalar_type>, "unsupported Redis scalar type");
    }
}

template <typename entity_type, typename fn_type, std::size_t... i>
void for_each_redis_field_impl(fn_type& function, std::index_sequence<i...>) {
    (function.template operator()<redis_entity_field_adapter<
            std::tuple_element_t<i, typename entity_type::columns_type>>>(),
        ...);
}

template <typename entity_type, typename fn_type>
void for_each_redis_field(fn_type&& function) {
    validate_redis_entity<entity_type>();
    for_each_redis_field_impl<entity_type>(function,
        std::make_index_sequence<std::tuple_size_v<typename entity_type::columns_type>>{});
}

}  // namespace ruvia::detail
