#pragma once

#include <algorithm>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

inline constexpr std::string_view kDefaultCapabilityAlias = "default";

inline void validateCapabilityAlias(std::string_view alias, const char* emptyMessage) {
    if (alias.empty()) {
        throw std::invalid_argument(emptyMessage);
    }
}

// Every named worker capability has the same startup representation: an alias
// plus configuration normalized into the owning PMR domain.
template <typename Storage>
struct NamedCapabilityDefinition final {
    using ConfigStorage = Storage;

    std::pmr::string alias;
    Storage config;
};

// Validates and normalizes the replacement before mutating retained App state.
// Existing aliases preserve registration order; new aliases take one owned PMR
// copy of the name and configuration.
template <typename Definition, typename SourceConfig>
void upsertNamedCapabilityDefinition(std::pmr::vector<Definition>& definitions,
    std::string_view alias, const SourceConfig& sourceConfig, const char* emptyMessage,
    std::pmr::memory_resource* resource) {
    validateCapabilityAlias(alias, emptyMessage);
    auto* const resolved = pmrResourceOrDefault(resource);
    typename Definition::ConfigStorage storedConfig(sourceConfig, resolved);
    for (auto& definition : definitions) {
        if (std::string_view(definition.alias) == alias) {
            definition.config = std::move(storedConfig);
            return;
        }
    }
    definitions.push_back(Definition{std::pmr::string(alias, resolved), std::move(storedConfig)});
}

// Validates the complete startup alias set without allocating. Capability
// owners call this before constructing pools, so an invalid later definition
// cannot leave a partially built worker capability graph behind.
template <typename Entries>
void validateCapabilityAliases(
    const Entries& entries, const char* emptyMessage, const char* duplicateMessage) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::string_view alias = entries[index].alias;
        validateCapabilityAlias(alias, emptyMessage);
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (std::string_view(entries[previous].alias) == alias) {
                throw std::invalid_argument(duplicateMessage);
            }
        }
    }
}

// Immutable startup-built index shared by worker-local named capabilities.
// Entries retain ownership of alias text and their order stays fixed after
// build; the index binds those aliases to their original positions so lookup
// cannot accidentally use a different entry set. Request-time lookup performs
// one allocation-free binary search.
class NamedCapabilityIndex final {
public:
    explicit NamedCapabilityIndex(std::pmr::memory_resource* resource)
        : aliases_(pmrResourceOrDefault(resource)) {}

    template <typename Entries>
    void build(const Entries& entries) {
        if (built_) {
            throw std::logic_error("named capability index may only be built once");
        }
        std::pmr::vector<IndexedAlias> aliases(aliases_.get_allocator().resource());
        aliases.reserve(entries.size());
        std::optional<std::size_t> defaultIndex;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const std::string_view alias = entries[index].alias;
            aliases.push_back(IndexedAlias{alias, index});
            if (alias == kDefaultCapabilityAlias) {
                defaultIndex = index;
            }
        }
        std::ranges::sort(aliases, {}, &IndexedAlias::alias);
        aliases_.swap(aliases);
        defaultIndex_ = defaultIndex;
        built_ = true;
    }

    [[nodiscard]] std::optional<std::size_t> find(std::string_view alias) const noexcept {
        const auto match = std::ranges::lower_bound(aliases_, alias, {}, &IndexedAlias::alias);
        if (match == aliases_.end() || match->alias != alias) {
            return std::nullopt;
        }
        return match->index;
    }

    [[nodiscard]] std::optional<std::size_t> defaultIndex() const noexcept {
        return defaultIndex_;
    }

private:
    struct IndexedAlias final {
        std::string_view alias;
        std::size_t index;
    };

    std::pmr::vector<IndexedAlias> aliases_;
    std::optional<std::size_t> defaultIndex_;
    bool built_{false};
};

}  // namespace ruvia::detail
