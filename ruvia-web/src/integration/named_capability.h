#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"

namespace ruvia::detail {

inline constexpr std::string_view default_capability_alias = "default";

inline void validate_capability_alias(std::string_view alias, const char* empty_message) {
    if (alias.empty()) {
        throw std::invalid_argument(empty_message);
    }
}

// Every named worker capability has the same startup representation: an alias
// plus configuration normalized into the owning PMR domain.
template <typename storage_type>
struct named_capability_definition final {
    using config_storage_type = storage_type;

    std::pmr::string alias_;
    storage_type config_;
};

// Validates and normalizes the replacement before mutating retained application state.
// Existing aliases preserve registration order; new aliases take one owned PMR
// copy of the name and configuration.
template <typename definition_type, typename source_config_type>
void upsert_named_capability_definition(std::pmr::vector<definition_type>& definitions,
    std::string_view alias, const source_config_type& source_config, const char* empty_message,
    std::pmr::memory_resource* resource) {
    validate_capability_alias(alias, empty_message);
    auto* const resolved = pmr_resource_or_default(resource);
    typename definition_type::config_storage_type stored_config(source_config, resolved);
    for (auto& definition : definitions) {
        if (std::string_view(definition.alias_) == alias) {
            definition.config_ = std::move(stored_config);
            return;
        }
    }
    definitions.push_back(definition_type{std::pmr::string(alias, resolved), std::move(stored_config)});
}

// Validates the complete startup alias set without allocating. Capability
// owners call this before constructing pools, so an invalid later definition
// cannot leave a partially built worker capability graph behind.
template <typename entries_type>
void validate_capability_aliases(
    const entries_type& entries, const char* empty_message, const char* duplicate_message) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::string_view alias = entries[index].alias_;
        validate_capability_alias(alias, empty_message);
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (std::string_view(entries[previous].alias_) == alias) {
                throw std::invalid_argument(duplicate_message);
            }
        }
    }
}

// Immutable startup-built index shared by worker-local named capabilities.
// It owns one PMR copy of each alias and binds it to the source registration
// position; source definitions need not outlive build(). Request-time lookup
// performs one allocation-free binary search.
class named_capability_index final {
public:
    explicit named_capability_index(std::pmr::memory_resource* resource)
        : aliases_(pmr_resource_or_default(resource)) {}

    void build(std::initializer_list<std::string_view> aliases) {
        build_aliases(aliases, [](std::string_view alias) noexcept { return alias; });
    }

    template <typename entries_type>
    void build(const entries_type& entries) {
        build_aliases(
            entries, [](const auto& entry_value) noexcept -> std::string_view { return entry_value.alias_; });
    }

    [[nodiscard]] std::optional<std::size_t> find(std::string_view alias) const noexcept {
        const auto match =
            std::ranges::lower_bound(aliases_, alias, {}, &named_capability_index::alias_view);
        if (match == aliases_.end() || match->alias_ != alias) {
            return std::nullopt;
        }
        return match->index_;
    }

    [[nodiscard]] std::optional<std::size_t> default_index() const noexcept {
        return default_index_;
    }

private:
    struct indexed_alias_type final {
        std::pmr::string alias_;
        std::size_t index_;
    };

    [[nodiscard]] static std::string_view alias_view(const indexed_alias_type& entry_value) noexcept {
        return entry_value.alias_;
    }

    template <typename entries_type, typename alias_projection_type>
    void build_aliases(const entries_type& entries, alias_projection_type alias_projection) {
        if (built_) {
            throw std::logic_error("named capability index may only be built once");
        }
        auto* const resource = aliases_.get_allocator().resource();
        std::pmr::vector<indexed_alias_type> aliases(resource);
        aliases.reserve(entries.size());
        std::optional<std::size_t> default_index;
        std::size_t index = 0;
        for (const auto& entry : entries) {
            const std::string_view alias = alias_projection(entry);
            aliases.push_back(indexed_alias_type{std::pmr::string(alias, resource), index});
            if (alias == default_capability_alias) {
                default_index = index;
            }
            ++index;
        }
        std::ranges::sort(aliases, {}, &named_capability_index::alias_view);
        aliases_.swap(aliases);
        default_index_ = default_index;
        built_ = true;
    }

    std::pmr::vector<indexed_alias_type> aliases_;
    std::optional<std::size_t> default_index_;
    bool built_{false};
};

}  // namespace ruvia::detail
