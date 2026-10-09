#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/request_fields.h"

namespace ruvia::detail {

// Request-query multivalue indexing is an implementation detail of
// context_request::queries(). It must not become a second public request-field
// model alongside request_name_value_list.
class request_query_values final {
public:
    class group_type final {
    public:
        group_type(std::pmr::memory_resource* resource, std::string_view name)
            : name_(name),
              values_(pmr_resource_or_default(resource)) {}

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

    explicit request_query_values(std::pmr::memory_resource* resource)
        : groups_(pmr_resource_or_default(resource)) {}

    void reserve(std::size_t count) {
        groups_.reserve(count);
    }

    [[nodiscard]] group_type& append(std::string_view name) {
        return groups_.emplace_back(groups_.get_allocator().resource(), name);
    }

    [[nodiscard]] std::span<const std::string_view> values(std::string_view name) const noexcept {
        for (auto it = groups_.rbegin(); it != groups_.rend(); ++it) {
            if (it->name() == name) {
                return it->values();
            }
        }
        return {};
    }

private:
    std::pmr::vector<group_type> groups_;
};

// The flattened scalar view and multivalue index are materialized together.
// One owner prevents context from representing a half-built query cache.
class request_query_cache final {
public:
    request_query_cache(std::pmr::vector<std::pmr::string>&& storage, request_name_value_list&& fields_value,
        request_query_values&& values) noexcept
        : storage_(std::move(storage)),
          fields_(std::move(fields_value)),
          values_(std::move(values)) {}

    [[nodiscard]] const request_name_value_list& fields() const& noexcept {
        return fields_;
    }
    const request_name_value_list& fields() const&& = delete;

    [[nodiscard]] const request_query_values& values() const& noexcept {
        return values_;
    }
    const request_query_values& values() const&& = delete;

private:
    // The public fields and multivalue groups borrow these decoded strings.
    // Keep storage first so reverse member destruction drops borrowers before
    // their backing bytes.
    std::pmr::vector<std::pmr::string> storage_;
    request_name_value_list fields_;
    request_query_values values_;
};

}  // namespace ruvia::detail
