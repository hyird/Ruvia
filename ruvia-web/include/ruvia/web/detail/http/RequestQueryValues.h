#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

// Request-query multivalue indexing is an implementation detail of
// ContextRequest::queries(). It must not become a second public request-field
// model alongside RequestNameValueList.
class RequestQueryValues final {
public:
    class Group final {
    public:
        Group(std::pmr::memory_resource* resource, std::string_view name)
            : name_(name),
              values_(pmrResourceOrDefault(resource)) {}

        void add(std::string_view value) {
            values_.push_back(value);
        }

        [[nodiscard]] std::string_view name() const noexcept {
            return name_;
        }

        [[nodiscard]] std::span<const std::string_view> values() const noexcept {
            return values_;
        }

    private:
        std::string_view name_;
        std::pmr::vector<std::string_view> values_;
    };

    explicit RequestQueryValues(std::pmr::memory_resource* resource)
        : groups_(pmrResourceOrDefault(resource)) {}

    void reserve(std::size_t count) {
        groups_.reserve(count);
    }

    [[nodiscard]] Group& append(std::string_view name) {
        return groups_.emplace_back(groups_.get_allocator().resource(), name);
    }

    [[nodiscard]] std::span<const std::string_view> values(
        std::string_view name) const noexcept {
        for (auto it = groups_.rbegin(); it != groups_.rend(); ++it) {
            if (it->name() == name) {
                return it->values();
            }
        }
        return {};
    }

private:
    std::pmr::vector<Group> groups_;
};

}  // namespace ruvia::detail
