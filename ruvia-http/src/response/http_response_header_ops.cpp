#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http_response.h"

#include "coding/http_content_coding.h"
#include "field/http_media_type.h"
#include "response/http_response_header_access.h"
#include "response/http_response_headers_access.h"
#include "response/response_header_index_cache.h"
#include "util/http_number_format.h"

namespace ruvia {
namespace {

void write_unsigned_header_value(http_response_header& header_value, std::uint64_t value) {
    auto* const begin = detail::response_header_value_begin(header_value);
    auto* const end = detail::response_header_value_end(header_value);
    const auto [ptr, ec] = std::to_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        throw std::logic_error("failed to format HTTP response header value");
    }
}

void validate_connection_control_field(std::string_view name, std::string_view value) {
    if (detail::http_ascii_equals_ignore_case(name, "Connection")) {
        detail::http_connection_options options;
        if (options.parse_field(
                value, detail::http_field_list_role::sender, [](std::string_view option) noexcept {
                    return !detail::http_connection_option_conflicts_with_managed_field(option);
                }) != detail::http_field_list_parse_status::ok) {
            throw std::invalid_argument("invalid HTTP Connection header");
        }
    } else if (detail::http_ascii_equals_ignore_case(name, "Upgrade")) {
        detail::http_upgrade_protocols protocols;
        if (protocols.parse_field(value, detail::http_field_list_role::sender,
                [](const detail::http_upgrade_protocol&) noexcept { return true; }) !=
            detail::http_field_list_parse_status::ok) {
            throw std::invalid_argument("invalid HTTP Upgrade header");
        }
    } else if (detail::http_ascii_equals_ignore_case(name, "TE")) {
        throw std::invalid_argument("TE is not a response field");
    }
}

[[nodiscard]] bool response_header_mode_appends(http_response_header_mode mode) {
    switch (mode) {
        case http_response_header_mode::replace:
            return false;
        case http_response_header_mode::append:
            return true;
    }
    throw std::invalid_argument("invalid HTTP response header mode");
}

}  // namespace

void http_response::record_known_header_index(std::uint32_t known_bit, std::size_t index) noexcept {
    known_header_bits_ |= known_bit;
    detail::record_response_header_index(
        known_header_indexes_, detail::response_known_header_slot(known_bit), index);
}

http_response_header* http_response::find_header_for_update(
    std::string_view key, std::uint32_t known_bit) noexcept {
    return const_cast<http_response_header*>(std::as_const(*this).find_header_for_read(key, known_bit));
}

const http_response_header* http_response::find_header_for_read(
    std::string_view key, std::uint32_t known_bit) const noexcept {
    const auto* const begin = headers_.begin();
    const auto* const end = headers_.end();
    const auto* const header_value = detail::find_response_header_indexed(
        begin, end, known_header_indexes_, detail::response_known_header_slot(known_bit), key, known_bit);
    return header_value == end ? nullptr : header_value;
}

http_response_header& http_response::prepare_header_value_storage(
    std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    if (auto* const header_value = find_header_for_update(key, known_bit)) {
        const bool was_appended = detail::response_header_append(*header_value);
        headers_.assign_uninitialized_value(*header_value, key, value_size, known_bit);
        return was_appended ? collapse_response_headers(*header_value, known_bit) : *header_value;
    }

    const auto index = headers_.size();
    auto& header_value = headers_.add_uninitialized_value(key, value_size, known_bit);
    record_known_header_index(known_bit, index);
    return header_value;
}

std::string_view http_response::known_header_value(std::uint32_t bit) const noexcept {
    const auto* const header_value = find_header_for_read({}, bit);
    return header_value == nullptr ? std::string_view{} : header_value->value();
}

std::optional<std::string_view> http_response::header(std::string_view name) const& noexcept {
    if (const auto* const found =
            find_header_for_read(name, detail::classify_response_header_name(name))) {
        return found->value();
    }
    return std::nullopt;
}

void http_response::rebuild_known_header_index() noexcept {
    known_header_bits_ = 0;
    known_header_indexes_.fill(detail::missing_response_header_index_slot);
    const auto* const begin = headers_.begin();
    const auto* const end = headers_.end();
    for (auto* cursor_value = begin; cursor_value != end; ++cursor_value) {
        const auto known_bit = detail::response_header_known_bit(*cursor_value);
        if (known_bit == 0) {
            continue;
        }
        known_header_bits_ |= known_bit;
        detail::record_response_header_index(known_header_indexes_,
            detail::response_known_header_slot(known_bit), static_cast<std::size_t>(cursor_value - begin));
    }
}

void http_response::header(std::string_view key, std::string_view value) {
    header(key, value, {});
}

void http_response::header(std::string_view key, std::string_view value, header_options_type options) {
    // Check the descriptor's representable storage before any grammar scan;
    // callers may provide a view over a bounded buffer with a hostile length.
    detail::validate_response_header_storage_size(key.size(), value.size());
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    if (!is_valid_http_header_value(value)) {
        throw std::invalid_argument("invalid HTTP header value");
    }
    validate_connection_control_field(key, value);
    const auto known_bit = detail::classify_response_header_name(key);
    if (known_bit == detail::response_header_content_type &&
        !detail::is_valid_http_content_type_field_value(value)) {
        throw std::invalid_argument("invalid HTTP Content-Type header");
    }
    if (known_bit == detail::response_header_content_encoding &&
        !detail::is_valid_http_content_encoding_field_value(value, detail::http_field_list_role::sender)) {
        throw std::invalid_argument("invalid HTTP Content-Encoding header");
    }
    if (detail::http_ascii_equals_ignore_case(key, "Trailer") &&
        !detail::is_valid_http_response_trailer_field_value(value, detail::http_field_list_role::sender)) {
        throw std::invalid_argument("invalid HTTP Trailer header");
    }
    if (response_header_mode_appends(options.mode_)) {
        if (detail::response_header_append_forbidden(known_bit)) {
            throw std::invalid_argument("HTTP response header cannot be appended");
        }
        if (known_bit == detail::response_header_set_cookie) {
            upsert_set_cookie_header_validated(value);
        } else {
            append_header_validated(key, value, known_bit);
        }
    } else {
        set_header_validated(key, value, known_bit);
    }
}

void http_response::remove_header(std::string_view key) {
    detail::validate_response_header_storage_size(key.size(), 0);
    if (!is_valid_http_header_name(key)) {
        throw std::invalid_argument("invalid HTTP header name");
    }
    (void)remove_header_validated(key, detail::classify_response_header_name(key));
}

void http_response::set_header_validated(
    std::string_view key, std::string_view value, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(key.size(), value.size());
    if (auto* const header = find_header_for_update(key, known_bit)) {
        const bool was_appended = detail::response_header_append(*header);
        headers_.assign(*header, key, value, known_bit);
        if (was_appended) {
            (void)collapse_response_headers(*header, known_bit);
        }
        return;
    }

    const auto index = headers_.size();
    headers_.add(key, value, known_bit);
    record_known_header_index(known_bit, index);
}

void http_response::append_header_validated(
    std::string_view key, std::string_view value, std::uint32_t known_bit) {
    auto& header_value = append_header_uninitialized_value(key, value.size(), known_bit);
    // Appending can move descriptors, but existing header byte blocks stay
    // stable, including any bytes borrowed by value.
    if (!value.empty()) {
        std::memcpy(detail::response_header_value_begin(header_value), value.data(), value.size());
    }
}

http_response_header& http_response::append_header_uninitialized_value(
    std::string_view key, std::size_t value_size, std::uint32_t known_bit) {
    detail::validate_response_header_storage_size(key.size(), value_size);
    if (detail::response_header_append_forbidden(known_bit)) {
        throw std::invalid_argument("HTTP response header cannot be appended");
    }
    // The index cache intentionally points at the first occurrence. Mark that
    // retained slot as plural too, so a later plain set can detect multiplicity
    // in O(1) and collapse the field without scanning every normal update. Do
    // not set it until the new descriptor has been published: the new header
    // owns bytes before it may allocate the backing table, and a failed append
    // must leave the existing response exactly as it was.
    const auto* const existing = find_header_for_read(key, known_bit);
    const auto existing_index = existing == nullptr
                                    ? std::optional<std::size_t>{}
                                    : std::optional{static_cast<std::size_t>(existing - headers_.begin())};
    const auto index = headers_.size();
    auto& header_value = headers_.add_uninitialized_value(key, value_size, known_bit);
    if (existing_index) {
        // Appending may move the descriptor table. Preserve an index, not a pointer,
        // to mark the existing entry without repeating the header lookup.
        detail::set_response_header_append(headers_.begin()[*existing_index], true);
    }
    // Mark the append flag so a later merge of this response keeps every appended
    // value instead of treating the field as single-valued and dropping all but the
    // first.
    detail::set_response_header_append(header_value, true);
    record_known_header_index(known_bit, index);
    return header_value;
}

http_response_header& http_response::collapse_response_headers(
    http_response_header& retained, std::uint32_t known_bit) noexcept {
    const auto key = retained.name();
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* const retained_address = &retained;
    auto* collapsed_retained = retained_address;
    auto* write = begin;
    for (auto* read = begin; read != end; ++read) {
        const auto header_known_bit = detail::response_header_known_bit(*read);
        const bool matches = known_bit != 0 ? header_known_bit == known_bit
                                            : detail::http_ascii_equals_ignore_case(read->name(), key);
        if (matches && read != retained_address) {
            headers_.release_header(*read);
            continue;
        }
        if (read == retained_address) {
            collapsed_retained = write;
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
    return *collapsed_retained;
}

bool http_response::remove_header_validated(std::string_view key, std::uint32_t known_bit) noexcept {
    if (known_bit != 0 && (known_header_bits_ & known_bit) == 0) {
        return false;
    }
    auto* const begin = headers_.begin();
    auto* const end = headers_.end();
    auto* write = begin;
    bool removed = false;
    http_response_header retired_key_header;

    for (auto* read = begin; read != end; ++read) {
        const auto header_known_bit = detail::response_header_known_bit(*read);
        const bool matches = known_bit != 0 ? header_known_bit == known_bit
                                            : detail::http_ascii_equals_ignore_case(read->name(), key);
        if (matches) {
            if (!removed) {
                retired_key_header = std::exchange(*read, http_response_header{});
                key = retired_key_header.name();
            } else {
                headers_.release_header(*read);
            }
            removed = true;
            continue;
        }
        if (write != read) {
            *write = *read;
        }
        ++write;
    }

    if (!removed) {
        return false;
    }

    headers_.release_header(retired_key_header);
    detail::http_response_headers_access::truncate(headers_, begin, write);
    rebuild_known_header_index();
    return true;
}

void http_response::header_stable_view(std::string_view key, std::string_view value) {
    detail::validate_response_header_storage_size(key.size(), value.size());
    const auto known_bit = detail::classify_response_header_name(key);
    if (auto* const header = find_header_for_update(key, known_bit)) {
        const bool was_appended = detail::response_header_append(*header);
        headers_.assign_stable_view(*header, key, value, known_bit);
        if (was_appended) {
            (void)collapse_response_headers(*header, known_bit);
        }
        return;
    }

    const auto index = headers_.size();
    headers_.add_stable_view(key, value, known_bit);
    record_known_header_index(known_bit, index);
}

void http_response::set_header_unsigned(
    std::string_view key, std::uint64_t value, std::uint32_t known_bit) {
    auto& header_value = prepare_header_value_storage(key, detail::http_unsigned_decimal_size(value), known_bit);
    write_unsigned_header_value(header_value, value);
}

void http_response::reserve_headers(std::size_t count) {
    headers_.reserve(count);
}

}  // namespace ruvia
