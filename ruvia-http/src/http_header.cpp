#include "http_header.h"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "http_field_kind.h"
#include "http_field_text.h"
#include "http_message_access.h"
#include "http_response.h"

namespace ruvia::detail {

namespace {

struct http_field_definition final {
    std::string_view name_;
    http_field_kind kind_;
    std::uint32_t response_bit_;
};

// One spelling authority for both directions. Length-indexed ranges keep
// classification bounded to the names of the same size without allocation.
constexpr http_field_definition http_field_definitions[] = {
    {"te", http_field_kind::te, 0},
    {"date", http_field_kind::other, response_header_date},
    {"etag", http_field_kind::other, response_header_etag},
    {"host", http_field_kind::host, 0},
    {"vary", http_field_kind::other, response_header_vary},
    {"allow", http_field_kind::other, response_header_allow},
    {"range", http_field_kind::range, 0},
    {"accept", http_field_kind::accept, 0},
    {"cookie", http_field_kind::cookie, 0},
    {"expect", http_field_kind::expect, 0},
    {"origin", http_field_kind::origin, 0},
    {"server", http_field_kind::other, response_header_server},
    {"trailer", http_field_kind::trailer, response_header_trailer},
    {"upgrade", http_field_kind::upgrade, response_header_upgrade},
    {"if-match", http_field_kind::if_match, 0},
    {"if-range", http_field_kind::if_range, 0},
    {"location", http_field_kind::other, response_header_location},
    {"forwarded", http_field_kind::forwarded, 0},
    {"connection", http_field_kind::connection, response_header_connection},
    {"keep-alive", http_field_kind::other, response_header_keep_alive},
    {"set-cookie", http_field_kind::other, response_header_set_cookie},
    {"user-agent", http_field_kind::user_agent, 0},
    {"content-type", http_field_kind::content_type, response_header_content_type},
    {"accept-ranges", http_field_kind::other, response_header_accept_ranges},
    {"authorization", http_field_kind::authorization, 0},
    {"cache-control", http_field_kind::other, response_header_cache_control},
    {"content-range", http_field_kind::other, response_header_content_range},
    {"if-none-match", http_field_kind::if_none_match, 0},
    {"last-modified", http_field_kind::other, response_header_last_modified},
    {"content-length", http_field_kind::content_length, response_header_content_length},
    {"accept-encoding", http_field_kind::accept_encoding, 0},
    {"x-forwarded-for", http_field_kind::x_forwarded_for, 0},
    {"content-encoding", http_field_kind::content_encoding, response_header_content_encoding},
    {"proxy-connection", http_field_kind::other, response_header_proxy_connection},
    {"if-modified-since", http_field_kind::if_modified_since, 0},
    {"sec-websocket-key", http_field_kind::sec_websocket_key, 0},
    {"transfer-encoding", http_field_kind::transfer_encoding, response_header_transfer_encoding},
    {"x-forwarded-proto", http_field_kind::x_forwarded_proto, 0},
    {"if-unmodified-since", http_field_kind::if_unmodified_since, 0},
    {"proxy-authorization", http_field_kind::proxy_authorization, 0},
    {"sec-websocket-version", http_field_kind::sec_websocket_version, 0},
    {"access-control-max-age", http_field_kind::other, response_header_access_control_max_age},
    {"sec-websocket-protocol", http_field_kind::sec_websocket_protocol, 0},
    {"sec-websocket-extensions", http_field_kind::sec_websocket_extensions, 0},
    {"access-control-allow-origin", http_field_kind::other, response_header_access_control_allow_origin},
    {"access-control-allow-headers", http_field_kind::other, response_header_access_control_allow_headers},
    {"access-control-allow-methods", http_field_kind::other, response_header_access_control_allow_methods},
    {"access-control-expose-headers", http_field_kind::other, response_header_access_control_expose_headers},
    {"access-control-request-method", http_field_kind::access_control_request_method, 0},
    {"access-control-request-headers", http_field_kind::access_control_request_headers, 0},
    {"access-control-allow-credentials", http_field_kind::other, response_header_access_control_allow_credentials},
};

static_assert([] {
    for (std::size_t i = 1; i < std::size(http_field_definitions); ++i) {
        if (http_field_definitions[i - 1].name_.size() > http_field_definitions[i].name_.size()) {
            return false;
        }
    }
    return true;
}());

constexpr auto http_field_length_offsets = [] {
    std::array<std::size_t, http_field_definitions[std::size(http_field_definitions) - 1].name_.size() + 2> offsets{};
    std::size_t index = 0;
    for (std::size_t length = 0; length < offsets.size(); ++length) {
        while (index < std::size(http_field_definitions) && http_field_definitions[index].name_.size() < length) {
            ++index;
        }
        offsets[length] = index;
    }
    return offsets;
}();

constexpr auto response_field_names = [] {
    std::array<std::string_view, std::bit_width(response_header_proxy_connection)> names{};
    for (const auto& field : http_field_definitions) {
        if (field.response_bit_ != 0) {
            names[std::countr_zero(field.response_bit_)] = field.name_;
        }
    }
    return names;
}();

}  // namespace

http_field_classification classify_http_field_name(std::string_view name) noexcept {
    if (name.empty() || name.size() >= http_field_length_offsets.size() - 1) {
        return {};
    }
    const auto first = http_ascii_to_lower(static_cast<unsigned char>(name.front()));
    const auto end = http_field_length_offsets[name.size() + 1];
    for (auto index = http_field_length_offsets[name.size()]; index < end; ++index) {
        const auto& field = http_field_definitions[index];
        if (first == static_cast<unsigned char>(field.name_.front()) && http_ascii_equals_ignore_case(name, field.name_)) {
            return {field.kind_, field.response_bit_};
        }
    }
    return {};
}

http_field_kind classify_http_field(std::string_view name) noexcept {
    return classify_http_field_name(name).kind_;
}

std::uint32_t classify_response_header_name(std::string_view name) noexcept {
    return classify_http_field_name(name).response_bit_;
}

std::string_view response_known_header_lowercase_name(std::uint32_t bit) noexcept {
    if (!std::has_single_bit(bit)) {
        return {};
    }
    const auto index = static_cast<std::size_t>(std::countr_zero(bit));
    return index < response_field_names.size() ? response_field_names[index] : std::string_view{};
}

}  // namespace ruvia::detail

namespace ruvia {

std::optional<std::string_view> http_field_name_list::next() noexcept {
    while (!finished_) {
        const auto separator = remaining_.find(',');
        const auto member = remaining_.substr(0, separator);
        if (separator == std::string_view::npos) {
            finished_ = true;
        } else {
            remaining_.remove_prefix(separator + 1);
        }
        const auto name = http_trim_ows(member);
        if (name.empty()) {
            if (finished_) {
                valid_ = true;
            }
            continue;
        }
        if (!is_valid_http_header_name(name)) {
            finished_ = true;
            valid_ = false;
            return std::nullopt;
        }
        if (finished_) {
            valid_ = true;
        }
        return name;
    }
    return std::nullopt;
}

namespace {

template <typename target_type>
concept http_header_vector_target = std::same_as<target_type, std::vector<http_header>*>;

}  // namespace

http_header_range_view editable_http_headers::headers() const noexcept {
    // The view cannot be moved: each branch returns a prvalue.
    if (const auto* const lines = std::get_if<std::vector<http_header>*>(&target_)) {
        return http_header_range_view(**lines);
    }
    if (const auto* const block = std::get_if<http_response_headers*>(&target_)) {
        return http_header_range_view(**block);
    }
    if (const auto* const custom = std::get_if<custom_target>(&target_)) {
        return custom->fields_(custom->object_);
    }
    return http_header_range_view(*std::get<http_response*>(target_));
}

void editable_http_headers::remove(std::string_view name) noexcept {
    std::visit([name](auto target) noexcept {
        if constexpr (http_header_vector_target<decltype(target)>) {
            std::erase_if(*target, [name](const http_header& field) noexcept {
                return http_ascii_equals_ignore_case(field.name(), name);
            });
        } else if constexpr (std::same_as<decltype(target), custom_target>) {
            target.remove_(target.object_, name);
        } else if (is_valid_http_header_name(name) && detail::response_header_storage_size_fits(name.size(), 0)) {
            // A response block rejects a name no field can carry; no line has
            // such a name, so there is nothing to erase.
            target->remove_header(name);
        }
    }, target_);
}

void editable_http_headers::remove_if(bool (*matches)(std::string_view field_name) noexcept) noexcept {
    if (auto* const lines = std::get_if<std::vector<http_header>*>(&target_)) {
        std::erase_if(**lines, [matches](const http_header& field) noexcept {
            return matches(field.name());
        });
        return;
    }
    // A response block removes by name and tolerates a name borrowed from one
    // of its own lines. Each removal invalidates the view, so the scan
    // restarts until no line matches.
    for (bool removed = true; removed;) {
        removed = false;
        for (const auto field : headers()) {
            if (matches(field.name())) {
                remove(field.name());
                removed = true;
                break;
            }
        }
    }
}

void editable_http_headers::set(std::string_view name, std::string_view value) {
    std::visit([name, value](auto target) {
        if constexpr (http_header_vector_target<decltype(target)>) {
            // Built before any line is retired: name and value may borrow one.
            http_header replacement(name, value);
            const auto first = std::ranges::find_if(*target, [name](const http_header& field) noexcept {
                return http_ascii_equals_ignore_case(field.name(), name);
            });
            if (first == target->end()) {
                target->push_back(std::move(replacement));
                return;
            }
            *first = std::move(replacement);
            const auto retained_name = first->name();
            target->erase(std::remove_if(first + 1, target->end(), [retained_name](const http_header& field) noexcept {
                return http_ascii_equals_ignore_case(field.name(), retained_name);
            }),
                target->end());
        } else if constexpr (std::same_as<decltype(target), custom_target>) {
            target.set_(target.object_, name, value);
        } else {
            target->header(name, value);
        }
    }, target_);
}

void editable_http_headers::append(std::string_view name, std::string_view value) {
    std::visit([name, value](auto target) {
        if constexpr (http_header_vector_target<decltype(target)>) {
            target->emplace_back(name, value);
        } else if constexpr (std::same_as<decltype(target), custom_target>) {
            target.append_(target.object_, name, value);
        } else {
            target->header(name, value, {.mode_ = http_response_header_mode::append});
        }
    }, target_);
}

void editable_http_headers::run_transaction(void (*edit)(void* callable, editable_http_headers candidate), void* callable, std::size_t additional_fields) {
    if (auto* const response = std::get_if<http_response*>(&target_)) {
        auto candidate = (*response)->headers().clone(additional_fields);
        edit(callable, candidate);
        (*response)->commit_headers(std::move(candidate));
        return;
    }
    edit(callable, *this);
}

}  // namespace ruvia

namespace ruvia::detail {

bool is_valid_long_http_field_value_bytes(const char* data, std::size_t size) noexcept {
    return http_field_value_prefix_size({data, size}) == size;
}

}  // namespace ruvia::detail
