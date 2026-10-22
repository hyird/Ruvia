#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/context.h"

namespace ruvia::detail {

// A lazily built index for queries(name). Values stay in request order within
// each name; groups are sorted only for lookup and are never exposed publicly.
class request_query_values final {
public:
    request_query_values(const request_name_value_list& fields, std::pmr::memory_resource* resource)
        : groups_(resource),
          values_(resource) {
        std::pmr::vector<std::size_t> order(resource);
        order.reserve(fields.size());
        for (std::size_t i = 0; i < fields.size(); ++i) {
            order.push_back(i);
        }
        std::ranges::sort(order, [&fields](std::size_t left, std::size_t right) noexcept {
            const auto left_name = fields[left].name();
            const auto right_name = fields[right].name();
            return left_name == right_name ? left < right : left_name < right_name;
        });
        groups_.reserve(fields.size());
        values_.reserve(fields.size());
        for (const auto index : order) {
            const auto name = fields[index].name();
            if (groups_.empty() || groups_.back().name_ != name) {
                groups_.push_back({name, values_.size(), 0});
            }
            values_.push_back(fields[index].value());
            ++groups_.back().count_;
        }
    }

    [[nodiscard]] std::span<const std::string_view> values(std::string_view name) const noexcept {
        const auto group = std::ranges::lower_bound(groups_, name, {}, &group_type::name_);
        if (group == groups_.end() || group->name_ != name) {
            return {};
        }
        return std::span<const std::string_view>(values_).subspan(group->begin_, group->count_);
    }

private:
    struct group_type final {
        std::string_view name_;
        std::size_t begin_;
        std::size_t count_;
    };
    std::pmr::vector<group_type> groups_;
    std::pmr::vector<std::string_view> values_;
};

class request_query_cache final {
public:
    request_query_cache(std::pmr::vector<std::pmr::string>&& storage, request_name_value_list&& fields_value) noexcept
        : storage_(std::move(storage)),
          fields_(std::move(fields_value)) {}

    [[nodiscard]] const request_name_value_list& fields() const& noexcept {
        return fields_;
    }
    const request_name_value_list& fields() const&& = delete;

    [[nodiscard]] const request_query_values& values() & {
        if (!values_) {
            // Construct completely before publishing any spans. A failed
            // allocation leaves the scalar cache intact and permits a retry.
            values_.emplace(fields_, storage_.get_allocator().resource());
        }
        return *values_;
    }
    const request_query_values& values() && = delete;

private:
    // Both views borrow these decoded strings. No owner or index grows after
    // publishing views; member order destroys borrowers before their storage.
    std::pmr::vector<std::pmr::string> storage_;
    request_name_value_list fields_;
    std::optional<request_query_values> values_;
};

}  // namespace ruvia::detail
