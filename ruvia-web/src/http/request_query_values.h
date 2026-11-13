#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
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
    explicit request_query_values(const request_name_value_list& fields) {
        if (fields.empty()) {
            return;
        }
        static constexpr std::size_t inline_order_capacity = 32;
        std::array<std::size_t, inline_order_capacity> inline_order;
        std::vector<std::size_t> heap_order;
        std::span<std::size_t> order;
        if (fields.size() <= inline_order_capacity) {
            order = std::span<std::size_t>(inline_order.data(), fields.size());
        } else {
            heap_order.resize(fields.size());
            order = heap_order;
        }
        for (std::size_t i = 0; i < fields.size(); ++i) {
            order[i] = i;
        }
        std::ranges::sort(order, [&fields](std::size_t left, std::size_t right) noexcept {
            const auto left_name = fields[left].name();
            const auto right_name = fields[right].name();
            return left_name == right_name ? left < right : left_name < right_name;
        });
        std::size_t group_count = 1;
        for (std::size_t i = 1; i < order.size(); ++i) {
            if (fields[order[i]].name() != fields[order[i - 1]].name()) {
                ++group_count;
            }
        }
        groups_.reserve(group_count);
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

    [[nodiscard]] std::span<const std::string_view> values(
        std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND {
        const auto group = std::ranges::lower_bound(groups_, name, {}, &group_type::name_);
        if (group == groups_.end() || group->name_ != name) {
            return {};
        }
        return std::span<const std::string_view>(values_).subspan(group->begin_, group->count_);
    }
    std::span<const std::string_view> values(std::string_view) const&& = delete;

private:
    struct group_type final {
        std::string_view name_;
        std::size_t begin_;
        std::size_t count_;
    };
    std::vector<group_type> groups_;
    std::vector<std::string_view> values_;
};

// Field views borrow the request query string or decoded_bytes_, so neither
// moves once published.
class request_query_cache final {
public:
    request_query_cache() noexcept
        : fields_(request_name_value_list_access::make()) {}

    [[nodiscard]] std::string& decoded_bytes() noexcept {
        return decoded_bytes_;
    }
    [[nodiscard]] request_name_value_list& mutable_fields() noexcept {
        return fields_;
    }

    [[nodiscard]] const request_name_value_list& fields() const& noexcept {
        return fields_;
    }
    const request_name_value_list& fields() const&& = delete;

    [[nodiscard]] const request_query_values& values() & {
        if (!values_) {
            values_.emplace(fields_);
        }
        return *values_;
    }
    const request_query_values& values() && = delete;

private:
    std::string decoded_bytes_;
    request_name_value_list fields_;
    std::optional<request_query_values> values_;
};

}  // namespace ruvia::detail
