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

#include "ruvia/web/detail/redis/RedisEntityTraits.h"
#include "ruvia/web/redis/RedisTypes.h"

namespace ruvia::detail {

[[noreturn]] inline void throwInvalidRedisScalar(std::string_view message) {
    throw RedisError(RedisError::Code::kProtocolError, message);
}

template <typename T>
[[nodiscard]] std::pmr::string encodeRedisNumber(
    T value, std::pmr::memory_resource* resource) {
    char buffer[128]{};
    const auto result = [&] {
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(value)) {
                throwInvalidRedisScalar("non-finite Redis scalar");
            }
            return std::to_chars(buffer, std::end(buffer), value, std::chars_format::general,
                std::numeric_limits<T>::max_digits10);
        } else {
            return std::to_chars(buffer, std::end(buffer), value);
        }
    }();
    if (result.ec != std::errc{}) {
        throwInvalidRedisScalar("failed to encode Redis scalar");
    }
    return std::pmr::string(buffer, static_cast<std::size_t>(result.ptr - buffer), resource);
}

template <typename T>
[[nodiscard]] T decodeRedisNumber(std::string_view value) {
    T result{};
    const auto parsed = [&] {
        if constexpr (std::is_floating_point_v<T>) {
            return std::from_chars(value.data(), value.data() + value.size(), result,
                std::chars_format::general);
        } else {
            return std::from_chars(value.data(), value.data() + value.size(), result);
        }
    }();
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        (std::is_floating_point_v<T> && !std::isfinite(result))) {
        throwInvalidRedisScalar("malformed Redis scalar");
    }
    return result;
}

template <typename T>
[[nodiscard]] std::pmr::string encodeRedisScalar(
    const T& value, std::pmr::memory_resource* resource = nullptr) {
    auto* const memoryResource = pmrResourceOrDefault(resource);
    using Scalar = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Scalar, String>) {
        const auto text = value.view();
        return std::pmr::string(text.data(), text.size(), memoryResource);
    } else if constexpr (std::is_same_v<Scalar, std::pmr::string>) {
        return std::pmr::string(value.data(), value.size(), memoryResource);
    } else if constexpr (std::is_same_v<Scalar, std::string> ||
                         std::is_same_v<Scalar, std::string_view>) {
        return std::pmr::string(value.data(), value.size(), memoryResource);
    } else if constexpr (std::is_same_v<Scalar, bool>) {
        return std::pmr::string(value ? "1" : "0", memoryResource);
    } else if constexpr (std::is_same_v<Scalar, Bool>) {
        return std::pmr::string(static_cast<bool>(value) ? "1" : "0", memoryResource);
    } else if constexpr (isRuviaScalar<Scalar>) {
        using Raw = ModelScalarValueT<Scalar>;
        return encodeRedisNumber(static_cast<Raw>(value), memoryResource);
    } else if constexpr (isRedisEntityNativeNumber<Scalar>) {
        return encodeRedisNumber(value, memoryResource);
    } else {
        static_assert(alwaysFalse<Scalar>, "unsupported Redis scalar type");
    }
}

template <typename T>
[[nodiscard]] T decodeRedisScalar(
    std::string_view value, std::pmr::memory_resource* resource = nullptr) {
    auto* const memoryResource = pmrResourceOrDefault(resource);
    using Scalar = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<Scalar, String>) {
        String result(ModelOptions{.resource = memoryResource});
        result.assignOwned(value);
        return result;
    } else if constexpr (std::is_same_v<Scalar, std::pmr::string>) {
        return std::pmr::string(value.data(), value.size(), memoryResource);
    } else if constexpr (std::is_same_v<Scalar, bool>) {
        if (value == "0") {
            return false;
        }
        if (value == "1") {
            return true;
        }
        throwInvalidRedisScalar("malformed Redis boolean scalar");
    } else if constexpr (std::is_same_v<Scalar, Bool>) {
        if (value == "0") {
            return Bool{false};
        }
        if (value == "1") {
            return Bool{true};
        }
        throwInvalidRedisScalar("malformed Redis boolean scalar");
    } else if constexpr (isRuviaScalar<Scalar>) {
        using Raw = ModelScalarValueT<Scalar>;
        return Scalar(decodeRedisNumber<Raw>(value));
    } else if constexpr (isRedisEntityNativeNumber<Scalar>) {
        return decodeRedisNumber<Scalar>(value);
    } else {
        static_assert(alwaysFalse<Scalar>, "unsupported Redis scalar type");
    }
}

template <typename Entity, typename Fn, std::size_t... I>
void forEachRedisFieldImpl(Fn& function, std::index_sequence<I...>) {
    (function.template operator()<RedisEntityFieldAdapter<
            std::tuple_element_t<I, typename Entity::Columns>>>(),
        ...);
}

template <typename Entity, typename Fn>
void forEachRedisField(Fn&& function) {
    validateRedisEntity<Entity>();
    forEachRedisFieldImpl<Entity>(function,
        std::make_index_sequence<std::tuple_size_v<typename Entity::Columns>>{});
}

}  // namespace ruvia::detail
