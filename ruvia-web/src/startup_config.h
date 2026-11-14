#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "borrowed_text.h"
#include "ip_address.h"
#include "stop_token.h"

// Startup-only configuration checks and the named worker capability
// registry (HTTP client, database and Redis aliases). Nothing here runs on the
// request path except named_capability_index::find.
namespace ruvia::detail {

inline constexpr std::string_view default_capability_alias = "default";

inline void validate_capability_alias(std::string_view alias, const char* empty_message) {
    if (alias.empty()) {
        throw std::invalid_argument(empty_message);
    }
}

// Every named worker capability has the same startup representation: an alias
// plus validated, normalized configuration storage.
template <typename storage_type>
struct named_capability_definition final {
    using config_storage_type = storage_type;

    std::string alias_;
    storage_type config_;
};

// Replaces the definition registered under replacement.alias_ in place, so
// existing aliases preserve registration order, or appends a new alias.
template <typename definition_type>
void upsert_named_capability_definition(std::vector<definition_type>& definitions, definition_type&& replacement) {
    const std::string_view alias = replacement.alias_;
    for (auto& definition : definitions) {
        if (std::string_view(definition.alias_) == alias) {
            std::destroy_at(&definition);
            std::construct_at(&definition, std::move(replacement));
            return;
        }
    }
    definitions.push_back(std::move(replacement));
}

// Validates and normalizes the replacement before mutating retained application
// state; the new definition takes one owned copy of the name and configuration.
template <typename definition_type, typename source_config_type>
void upsert_named_capability_definition(std::vector<definition_type>& definitions, std::string_view alias, const source_config_type& source_config, const char* empty_message) {
    validate_capability_alias(alias, empty_message);
    upsert_named_capability_definition(definitions, definition_type{std::string(alias), typename definition_type::config_storage_type(source_config)});
}

// Validates the complete startup alias set without allocating. Capability
// owners call this before constructing pools, so an invalid later definition
// cannot leave a partially built worker capability graph behind.
template <typename entries_type>
void validate_capability_aliases(const entries_type& entries, const char* empty_message, const char* duplicate_message) {
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
// It owns one copy of each alias and binds it to the source registration
// position; source definitions need not outlive build(). Request-time lookup
// performs one allocation-free binary search.
class named_capability_index final {
public:
    named_capability_index() = default;
    void build(std::initializer_list<std::string_view> aliases) {
        build_aliases(aliases, [](std::string_view alias) noexcept {
            return alias;
        });
    }

    template <typename entries_type>
    void build(const entries_type& entries) {
        build_aliases(entries, [](const auto& entry_value) noexcept -> std::string_view {
            return entry_value.alias_;
        });
    }

    [[nodiscard]] std::optional<std::size_t> find(std::string_view alias) const noexcept {
        const auto match = std::ranges::lower_bound(aliases_, alias, {}, &named_capability_index::alias_view);
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
        std::string alias_;
        std::size_t index_;
    };

    [[nodiscard]] static std::string_view alias_view(const indexed_alias_type& entry_value) noexcept {
        return entry_value.alias_;
    }

    template <typename entries_type, typename alias_projection_type>
    void build_aliases(const entries_type& entries, alias_projection_type alias_projection) {
        std::vector<indexed_alias_type> aliases;
        aliases.reserve(entries.size());
        std::optional<std::size_t> default_index;
        std::size_t index = 0;
        for (const auto& entry : entries) {
            const std::string_view alias = alias_projection(entry);
            aliases.push_back(indexed_alias_type{std::string(alias), index});
            if (alias == default_capability_alias) {
                default_index = index;
            }
            ++index;
        }
        std::ranges::sort(aliases, {}, &named_capability_index::alias_view);
        aliases_.swap(aliases);
        default_index_ = default_index;
    }

    std::vector<indexed_alias_type> aliases_;
    std::optional<std::size_t> default_index_;
};

// A configured host whose port is a separate field: brackets and a lone colon
// (host:port) are rejected; an unbracketed IPv6 literal has several colons.
inline void ensure_config_host(std::string_view host, const char* empty_message, const char* invalid_message) {
    if (host.empty()) {
        throw std::invalid_argument(empty_message);
    }
    std::size_t colon_count = 0;
    for (const auto ch : host) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte == 0x7F || byte == '/' || byte == '\\' || byte == '[' || byte == ']') {
            throw std::invalid_argument(invalid_message);
        }
        colon_count += byte == ':' ? 1 : 0;
    }
    if (colon_count == 1) {
        throw std::invalid_argument(invalid_message);
    }
}

inline void ensure_positive_size(std::size_t value, const char* message) {
    if (value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_optional_size(const std::optional<std::size_t>& value, const char* message) {
    if (value.has_value() && *value == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_non_zero_port(std::uint16_t port, const char* message) {
    if (port == 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_non_zero_optional_port(const std::optional<port_number>& port, const char* message) {
    if (port.has_value()) {
        ensure_non_zero_port(port->value(), message);
    }
}

template <typename rep_type, typename period_type>
void ensure_positive_duration(std::chrono::duration<rep_type, period_type> value, const char* message) {
    if (value.count() <= 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_duration(timeout_duration value, const char* message) {
    ensure_positive_duration(value.value(), message);
}

template <typename rep_type, typename period_type>
void ensure_positive_optional_duration(const std::optional<std::chrono::duration<rep_type, period_type>>& value, const char* message) {
    if (value.has_value() && value->count() <= 0) {
        throw std::invalid_argument(message);
    }
}

inline void ensure_positive_optional_duration(const std::optional<timeout_duration>& value, const char* message) {
    if (value.has_value()) {
        ensure_positive_duration(*value, message);
    }
}

template <typename first_duration_type, typename... rest_durations_type>
void ensure_positive_optional_durations(const char* message, const first_duration_type& first, const rest_durations_type&... rest) {
    ensure_positive_optional_duration(first, message);
    (ensure_positive_optional_duration(rest, message), ...);
}

}  // namespace ruvia::detail
