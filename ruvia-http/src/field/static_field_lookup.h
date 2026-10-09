#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace ruvia::detail {

struct static_field_match final {
    std::uint8_t name_index_;
    std::optional<std::uint8_t> exact_index_;
};

// The wire table remains authoritative. This immutable index stores only its
// zero-based positions, preserving the first name and exact-value matches.
template <const auto& entries>
class static_field_lookup final {
    static constexpr auto no_index = std::numeric_limits<std::uint8_t>::max();
    static constexpr auto bucket_count = std::bit_ceil(entries.size() * 2);
    static constexpr auto max_name_size = [] {
        std::size_t size = 0;
        for (const auto& entry : entries) {
            if (entry.name_.size() > size) {
                size = entry.name_.size();
            }
        }
        return size;
    }();
    static_assert(!entries.empty() && entries.size() < no_index);
    static_assert([] {
        for (const auto& entry : entries) {
            if (entry.name_.empty()) {
                return false;
            }
        }
        return true;
    }());

public:
    consteval static_field_lookup() {
        buckets_.fill(no_index);
        next_.fill(no_index);
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto name = entries[index].name_;
            auto bucket = name_bucket(name);
            while (buckets_[bucket] != no_index && entries[buckets_[bucket]].name_ != name) {
                bucket = (bucket + 1) & (bucket_count - 1);
            }
            if (buckets_[bucket] == no_index) {
                buckets_[bucket] = static_cast<std::uint8_t>(index);
            } else {
                auto previous = buckets_[bucket];
                while (next_[previous] != no_index) {
                    previous = next_[previous];
                }
                next_[previous] = static_cast<std::uint8_t>(index);
            }
        }
    }

    // A missing value requests only a name match, as needed for never-indexed
    // QPACK fields. Empty values still request an exact match. Borrow the
    // optional across codec adapters instead of copying its payload again.
    [[nodiscard]] std::optional<static_field_match> find(
        std::string_view name, const std::optional<std::string_view>& value) const noexcept {
        if (name.empty() || name.size() > max_name_size) {
            return std::nullopt;
        }
        auto bucket = name_bucket(name);
        while (buckets_[bucket] != no_index && entries[buckets_[bucket]].name_ != name) {
            bucket = (bucket + 1) & (bucket_count - 1);
        }
        const auto first = buckets_[bucket];
        if (first == no_index) {
            return std::nullopt;
        }
        if (value) {
            for (auto current = first; current != no_index; current = next_[current]) {
                if (entries[current].value_ == *value) {
                    return static_field_match{first, current};
                }
            }
        }
        return static_field_match{first, std::nullopt};
    }

private:
    // Only discriminating bytes are hashed; the full name is always compared.
    [[nodiscard]] static constexpr std::size_t name_bucket(std::string_view name) noexcept {
        auto hash = static_cast<std::uint32_t>(name.size()) * 9U;
        hash ^= static_cast<unsigned char>(name.front());
        hash ^= static_cast<unsigned char>(name.back()) * 9U;
        return hash & (bucket_count - 1);
    }

    std::array<std::uint8_t, bucket_count> buckets_{};
    std::array<std::uint8_t, entries.size()> next_{};
};

}  // namespace ruvia::detail
