#include <cstddef>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_set_cookie.h"
#include "ruvia/http/http_set_cookie_plan.h"

#include "response/http_response_header_access.h"
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
    std::string_view domain, bool partitioned) noexcept {
    const auto parsed_value = parse_set_cookie(value);
    return parsed_value.has_value() && is_valid_http_header_name(parsed_value->name()) &&
           set_cookie_wire_name_matches(parsed_value->name(), wire_prefix, cookie_name) &&
           parsed_value->has(http_set_cookie_attribute::path) == has_path &&
           parsed_value->has(http_set_cookie_attribute::partitioned) == partitioned &&
           (!has_path || parsed_value->path() == path) &&
           http_ascii_equals_ignore_case(parsed_value->domain(), domain);
}

}  // namespace

void http_response_headers::set_cookie(const set_cookie_plan& plan) {
    auto* retained = find_set_cookie_header(
        plan.wire_prefix(), plan.name(), !plan.path().empty(), plan.path(), plan.domain(), plan.partitioned_);
    if (retained == nullptr) {
        auto& header_value = append_header_uninitialized_value(
            "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        plan.write(detail::http_response_header_access::value_begin(header_value));
        return;
    }

    const bool borrows_retained =
        detail::response_header_storage_overlaps(*retained, plan.name_) ||
        detail::response_header_storage_overlaps(*retained, plan.value_) ||
        detail::response_header_storage_overlaps(*retained, plan.path_) ||
        detail::response_header_storage_overlaps(*retained, plan.domain_);
    set_cookie_plan::written_fields written;
    if (borrows_retained) {
        auto prepared = make_uninitialized_header(
            "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::http_response_header_access::value_begin(prepared));
        release_header(*retained);
        *retained = prepared;
    } else {
        assign_uninitialized_value(
            *retained, "Set-Cookie", plan.size(), detail::response_header_set_cookie);
        written = plan.write_fields(detail::http_response_header_access::value_begin(*retained));
    }
    detail::http_response_header_access::set_append(*retained, true);
    erase_later_set_cookie_headers(*retained, written.wire_name_, !written.path_.empty(),
        written.path_, written.domain_, plan.partitioned_);
}

void http_response_headers::upsert_set_cookie_header_validated(std::string_view value) {
    const auto parsed_value = parse_set_cookie(value);
    const auto cookie_name = parsed_value.has_value() && is_valid_http_header_name(parsed_value->name())
                                 ? parsed_value->name()
                                 : std::string_view{};
    if (cookie_name.empty()) {
        append_header_validated("Set-Cookie", value, detail::response_header_set_cookie);
        return;
    }

    const auto has_path = parsed_value->has(http_set_cookie_attribute::path);
    const auto partitioned = parsed_value->has(http_set_cookie_attribute::partitioned);
    auto* retained = find_set_cookie_header({}, cookie_name, has_path, parsed_value->path(), parsed_value->domain(), partitioned);
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
    assign(*retained, "Set-Cookie", value, detail::response_header_set_cookie);
    detail::http_response_header_access::set_append(*retained, true);
    const auto copied_value = retained->value();
    erase_later_set_cookie_headers(*retained, copied_value.substr(name_offset, name_size), has_path,
        copied_value.substr(path_offset, path_size), copied_value.substr(domain_offset, domain_size), partitioned);
}

http_response_header* http_response_headers::find_set_cookie_header(std::string_view wire_prefix,
    std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain, bool partitioned) noexcept {
    for (auto& header : *this) {
        if (detail::http_response_header_access::known_bit(header) == detail::response_header_set_cookie &&
            set_cookie_value_matches_storage_key(
                header.value(), wire_prefix, cookie_name, has_path, path, domain, partitioned)) {
            return &header;
        }
    }
    return nullptr;
}

void http_response_headers::erase_later_set_cookie_headers(http_response_header& retained,
    std::string_view cookie_name, bool has_path, std::string_view path,
    std::string_view domain, bool partitioned) noexcept {
    // A response might already contain duplicates introduced through the raw
    // header API. Once an authoritative cookie path owns this storage key,
    // collapse every later occurrence so the final response has one value.
    auto* const end_value = end();
    auto* write = &retained + 1;
    for (auto* read = write; read != end_value; ++read) {
        if (detail::http_response_header_access::known_bit(*read) == detail::response_header_set_cookie &&
            set_cookie_value_matches_storage_key(
                read->value(), {}, cookie_name, has_path, path, domain, partitioned)) {
            release_header(*read);
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }
    if (write != end_value) {
        truncate(write);
    }
    rebuild_known_header_index();
}

}  // namespace ruvia
