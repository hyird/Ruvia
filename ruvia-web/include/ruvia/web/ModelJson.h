#pragma once

#include "ruvia/web/ModelTypes.h"
#include "ruvia/web/detail/model/parse/JsonWriter.h"
#include "ruvia/core/memory/PmrResource.h"

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia {

namespace detail {
class ValidatedModelBindings;
}

// A request-scoped view of one validated JSON body. The typed value and the
// exact original bytes share the same middleware scope, allowing JSONB
// passthrough without a parse/serialize round trip.
template <typename T>
class ValidatedJson final {
public:
    [[nodiscard]] const T& value() const noexcept {
        return *value_;
    }

    [[nodiscard]] std::string_view raw() const noexcept {
        return raw_;
    }

private:
    friend class detail::ValidatedModelBindings;

    ValidatedJson(const T& value, std::string_view raw) noexcept
        : value_(&value),
          raw_(raw) {}

    const T* value_;
    std::string_view raw_;
};

template <typename T>
[[nodiscard]] std::optional<T> fromJson(std::string_view body, std::pmr::memory_resource* resource = nullptr) {
    static_assert(JsonBody<T>::value, "fromJson<T> requires a RUVIA_MODEL");
    return JsonBody<T>::parseOwned(body, detail::pmrResourceOrDefault(resource));
}

template <typename T, typename Traits, typename Allocator>
std::optional<T> fromJson(std::basic_string<char, Traits, Allocator>&&, std::pmr::memory_resource* = nullptr) = delete;

template <typename T, typename Traits, typename Allocator>
std::optional<T> fromJson(const std::basic_string<char, Traits, Allocator>&&, std::pmr::memory_resource* = nullptr) = delete;

template <typename T>
void appendJson(std::pmr::string& output, const T& value) {
    output.reserve(output.size() + detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
}

template <typename T>
[[nodiscard]] std::pmr::string toJson(const T& value, std::pmr::memory_resource* resource = nullptr) {
    std::pmr::string output(detail::pmrResourceOrDefault(resource));
    output.reserve(detail::jsonSizeHintValue(value));
    detail::appendJsonValue(output, value);
    return output;
}

}  // namespace ruvia
