#include <limits>
#include <stdexcept>

#include "http2/http2_hpack.h"
#include "http2/http2_hpack_static_table.h"

namespace ruvia::detail {

namespace {

constexpr std::size_t max_hpack_string_length = std::numeric_limits<std::uint32_t>::max();

void validate_hpack_string_length(std::string_view value) {
    if (value.size() > max_hpack_string_length) {
        throw std::length_error("HPACK string literal exceeds the uint32 length limit");
    }
}

}  // namespace

void hpack_encoder::encode_integer(std::pmr::string& out, std::uint8_t first_byte_mask,
    std::uint8_t prefix_bits, std::uint32_t value) {
    const auto prefix_max = static_cast<std::uint32_t>((1U << prefix_bits) - 1U);
    if (value < prefix_max) {
        out.push_back(static_cast<char>(first_byte_mask | static_cast<std::uint8_t>(value)));
        return;
    }

    out.push_back(static_cast<char>(first_byte_mask | static_cast<std::uint8_t>(prefix_max)));
    value -= prefix_max;
    while (value >= 128) {
        out.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

void hpack_encoder::encode_string(std::pmr::string& out, std::string_view value) {
    validate_hpack_string_length(value);
    encode_integer(out, 0, 7, static_cast<std::uint32_t>(value.size()));
    out.append(value.data(), value.size());
}

void hpack_encoder::encode_indexed(std::pmr::string& out, std::uint32_t index) {
    encode_integer(out, 0x80, 7, index);
}

void hpack_encoder::encode_dynamic_table_size_update(std::pmr::string& out, std::uint32_t maximum) {
    // RFC 7541 §6.3: 001xxxxx followed by an HPACK integer with a 5-bit prefix.
    encode_integer(out, 0x20, 5, maximum);
}

void hpack_encoder::encode_header(
    std::pmr::string& out, std::string_view name, std::string_view value) {
    // Validate both literals before static-table matching or writing the field
    // prefix. A string_view can be larger than HPACK's uint32 length domain, and
    // rejecting it here keeps the output unchanged when the input is invalid.
    validate_hpack_string_length(name);
    validate_hpack_string_length(value);
    const auto match = hpack_static_fields.find(name, value);
    if (match && match->exact_index_) {
        // A fully indexed static-table entry carries no field value on the wire, so
        // there is nothing for an intermediary to index; the never-indexed hint does
        // not apply.
        encode_indexed(out, static_cast<std::uint32_t>(*match->exact_index_) + 1);
        return;
    }

    const bool never_indexed = hpack_header_name_is_sensitive(name);
    if (match) {
        encode_header_with_name_index(out, static_cast<std::uint32_t>(match->name_index_) + 1, value, never_indexed);
        return;
    }
    encode_integer(
        out, never_indexed ? hpack_literal_never_indexed : hpack_literal_without_indexing, 4, 0);
    encode_string(out, name);
    encode_string(out, value);
}

void hpack_encoder::encode_header_with_name_index(
    std::pmr::string& out, std::uint32_t name_index, std::string_view value, bool never_indexed) {
    validate_hpack_string_length(value);
    encode_integer(
        out, never_indexed ? hpack_literal_never_indexed : hpack_literal_without_indexing, 4, name_index);
    encode_string(out, value);
}

void hpack_encoder::encode_status(std::pmr::string& out, http_status_code status) {
    switch (status.value()) {
        case http_status::ok.value():
            encode_indexed(out, hpack_static_index::status_ok);
            return;
        case http_status::no_content.value():
            encode_indexed(out, hpack_static_index::status_no_content);
            return;
        case http_status::partial_content.value():
            encode_indexed(out, hpack_static_index::status_partial_content);
            return;
        case http_status::not_modified.value():
            encode_indexed(out, hpack_static_index::status_not_modified);
            return;
        case http_status::bad_request.value():
            encode_indexed(out, hpack_static_index::status_bad_request);
            return;
        case http_status::not_found.value():
            encode_indexed(out, hpack_static_index::status_not_found);
            return;
        case http_status::internal_server_error.value():
            encode_indexed(out, hpack_static_index::status_internal_server_error);
            return;
        default:
            break;
    }

    const auto token = http_status_code_token(status);
    encode_header_with_name_index(out, hpack_static_index::status_ok, http_status_code_token_view(token));
}

}  // namespace ruvia::detail
