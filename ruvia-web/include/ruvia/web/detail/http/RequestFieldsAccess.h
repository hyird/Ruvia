#pragma once

#include <cstddef>
#include <memory_resource>
#include <string_view>
#include <utility>

#include "ruvia/web/RequestFields.h"

namespace ruvia::detail {

struct RequestNameValueViewAccess final {
    [[nodiscard]] static constexpr RequestNameValueView make(
        std::string_view name,
        std::string_view value) noexcept {
        return RequestNameValueView(name, value);
    }
};

struct RequestNameValueListAccess final {
    [[nodiscard]] static RequestNameValueList make(std::pmr::memory_resource* resource) {
        return RequestNameValueList(resource);
    }

    static void reserve(RequestNameValueList& list, std::size_t count) {
        list.reserve(count);
    }

    static void pushBack(RequestNameValueList& list, RequestNameValueView value) {
        list.pushBack(value);
    }
};

struct RequestValueGroupAccess final {
    [[nodiscard]] static RequestValueGroup make(
        std::pmr::memory_resource* resource,
        std::string_view name) {
        return RequestValueGroup(resource, name);
    }

    static void add(RequestValueGroup& group, std::string_view value) {
        group.add(value);
    }
};

struct RequestValueGroupListAccess final {
    [[nodiscard]] static RequestValueGroupList make(std::pmr::memory_resource* resource) {
        return RequestValueGroupList(resource);
    }

    static void reserve(RequestValueGroupList& list, std::size_t count) {
        list.reserve(count);
    }

    static void pushBack(RequestValueGroupList& list, RequestValueGroup value) {
        list.pushBack(std::move(value));
    }
};

}  // namespace ruvia::detail
