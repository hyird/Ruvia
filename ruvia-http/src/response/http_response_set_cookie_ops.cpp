#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/util/ascii_case.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_set_cookie.h"
#include "ruvia/http/http_set_cookie_plan.h"

#include "response/http_response_header_access.h"
#include "response/http_response_headers_access.h"
#include "response/response_header_index_cache.h"

// Set-Cookie is the one response field that neither replaces nor appends by
// field name: a second cookie for the same storage key (name, domain, path)
// replaces the earlier one while cookies of other scopes stay. These operations
// own that rule, including finding the earlier line and dropping the ones it
// shadows.

namespace ruvia {
namespace {

[[nodiscard]] bool set_cookie_wire_name_matches(
    std::string_view value, std::string_view wire_prefix, std::string_view cookie_name) noexcept {
    if (value.size() != wire_prefix.size() + cookie_name.size()) {
        return false;
    }
    if (!value.starts_with(wire_prefix)) {
        return false;
    }
    return value.substr(wire_prefix.size()) == cookie_name;
}

[[nodiscard]] bool set_cookie_value_matches_storage_key(std::string_view value,
    std::string_view wire_prefix, std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain) noexcept {
    const auto parsed_value = parse_set_cookie(value);
    return parsed_value.has_value() && is_valid_http_header_name(parsed_value->name()) &&
           set_cookie_wire_name_matches(parsed_value->name(), wire_prefix, cookie_name) &&
           parsed_value->has(http_set_cookie_attribute::path) == has_path &&
           (!has_path || parsed_value->path() == path) &&
           detail::http_ascii_equals_ignore_case(parsed_value->domain(), domain);
}

}  // namespace

void http_response::set_cookie(const set_cookie_plan& plan) {
    auto* retained = find_set_cookie_header(
        plan.wire_prefix(), plan.name(), !plan.path().empty(), plan.path(), plan.domain());
    if (retained == nullptr) {
        auto& header_value = append_header_uninitialized_value(
            "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        plan.write(detail::response_header_value_begin(header_value));
        return;
    }

    const bool borrows_retained =
        detail::response_header_storage_overlaps(*retained, plan.name_) ||
        detail::response_header_storage_overlaps(*retained, plan.value_) ||
        detail::response_header_storage_overlaps(*retained, plan.path_) ||
        detail::response_header_storage_overlaps(*retained, plan.domain_);
    set_cookie_plan::written_fields written;
    if (borrows_retained) {
        auto prepared = headers_.make_uninitialized_header(
            "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::response_header_value_begin(prepared));
        headers_.release_header(*retained);
        *retained = prepared;
    } else {
        headers_.assign_uninitialized_value(
            *retained, "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::response_header_value_begin(*retained));
    }
    detail::set_response_header_append(*retained, true);
    erase_later_set_cookie_headers(*retained, written.wire_name_, !written.path_.empty(),
        written.path_, written.domain_);
}

void http_response::upsert_set_cookie_header_validated(std::string_view value) {
    const auto parsed_value = parse_set_cookie(value);
    const auto cookie_name = parsed_value.has_value() && is_valid_http_header_name(parsed_value->name())
                                 ? parsed_value->name()
                                 : std::string_view{};
    if (cookie_name.empty()) {
        append_header_validated("Set-Cookie", value, detail::response_header_set_cookie);
        return;
    }

    const auto has_path = parsed_value->has(http_set_cookie_attribute::path);
    auto* retained = find_set_cookie_header({}, cookie_name, has_path, parsed_value->path(), parsed_value->domain());
    if (retained == nullptr) {
        append_header_validated("Set-Cookie", value, detail::response_header_set_cookie);
        return;
    }

    const auto name_size = cookie_name.size();
    const auto path_size = parsed_value->path().size();
    const auto domain_size = parsed_value->domain().size();
    const auto name_offset = static_cast<std::size_t>(cookie_name.data() - value.data());
    const auto path_offset = parsed_value->path().empty() ? std::size_t{0}
                                                          : static_cast<std::size_t>(parsed_value->path().data() - value.data());
    const auto domain_offset = parsed_value->domain().empty() ? std::size_t{0}
                                                              : static_cast<std::size_t>(parsed_value->domain().data() - value.data());
    headers_.assign(*retained, "Set-Cookie", value, detail::response_header_set_cookie);
    detail::set_response_header_append(*retained, true);
    const auto copied_value = retained->value();
    erase_later_set_cookie_headers(*retained, copied_value.substr(name_offset, name_size), has_path,
        copied_value.substr(path_offset, path_size), copied_value.substr(domain_offset, domain_size));
}

http_response_header* http_response::find_set_cookie_header(std::string_view wire_prefix,
    std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain) noexcept {
    for (auto& header : headers_) {
        if (detail::response_header_known_bit(header) == detail::response_header_set_cookie &&
            set_cookie_value_matches_storage_key(
                header.value(), wire_prefix, cookie_name, has_path, path, domain)) {
            return &header;
        }
    }
    return nullptr;
}

void http_response::erase_later_set_cookie_headers(http_response_header& retained,
    std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain) noexcept {
    // A response might already contain duplicates introduced through the raw
    // header API. Once an authoritative cookie path owns this storage key,
    // collapse every later occurrence so the final response has one value.
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* write = &retained + 1;
    for (auto* read = &retained + 1; read != end; ++read) {
        if (detail::response_header_known_bit(*read) == detail::response_header_set_cookie &&
            set_cookie_value_matches_storage_key(
                read->value(), {}, cookie_name, has_path, path, domain)) {
            headers_.release_header(*read);
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != end) {
        detail::http_response_headers_access::truncate(headers_, begin, write);
    }
    rebuild_known_header_index();
}

}  // namespace ruvia
