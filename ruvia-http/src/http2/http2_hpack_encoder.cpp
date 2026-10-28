#include <limits>
#include <stdexcept>

#include "ruvia/http/hpack.h"

#include "http2/http2_hpack.h"
#include "http2/http2_hpack_static_table.h"
#include "util/http_text_overlap.h"

namespace ruvia {

namespace {

constexpr std::size_t max_hpack_string_length = std::numeric_limits<std::uint32_t>::max();

void validate_hpack_string_length(std::string_view value) {
    if (value.size() > max_hpack_string_length) {
        throw std::length_error("HPACK string literal exceeds the uint32 length limit");
    }
}

[[nodiscard]] constexpr std::uint64_t hpack_integer_encoded_bytes(
    std::uint8_t prefix_bits, std::uint32_t value) noexcept {
    const auto prefix_max = static_cast<std::uint32_t>((1U << prefix_bits) - 1U);
    if (value < prefix_max) {
        return 1;
    }
    value -= prefix_max;
    std::uint64_t bytes = 2;
    while (value >= 128) {
        ++bytes;
        value >>= 7;
    }
    return bytes;
}

void reserve_hpack_literal(std::pmr::string& out, std::uint32_t name_index,
    std::string_view name, std::string_view value) {
    // A literal's prefixes use at most 13 bytes (two uint32 lengths and
    // the representation byte). Keep spare-capacity writes on a cheap path.
    if (static_cast<std::uint64_t>(name.size()) + value.size() + 13 <= out.capacity() - out.size()) {
        return;
    }
    // Both validated uint32 literals and their prefixes fit this wide sum,
    // including when size_t is 32 bits.
    auto bytes = hpack_integer_encoded_bytes(4, name_index) +
                 hpack_integer_encoded_bytes(7, static_cast<std::uint32_t>(value.size())) +
                 static_cast<std::uint64_t>(value.size());
    if (name_index == 0) {
        bytes += hpack_integer_encoded_bytes(7, static_cast<std::uint32_t>(name.size())) +
                 static_cast<std::uint64_t>(name.size());
    }
    if (bytes > out.max_size() - out.size()) {
        throw std::length_error("HPACK field exceeds the output size limit");
    }
    out.reserve(out.size() + static_cast<std::size_t>(bytes));
}

void encode_hpack_integer(std::pmr::string& out, std::uint8_t first_byte_mask,
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

void encode_hpack_string(std::pmr::string& out, std::string_view value) {
    validate_hpack_string_length(value);
    encode_hpack_integer(out, 0, 7, static_cast<std::uint32_t>(value.size()));
    out.append(value.data(), value.size());
}

void encode_unindexed_literal(std::pmr::string& out, bool never_indexed,
    std::uint32_t name_index, std::string_view name, std::string_view value) {
    reserve_hpack_literal(out, name_index, name, value);
    encode_hpack_integer(out,
        never_indexed ? detail::hpack_literal_never_indexed : detail::hpack_literal_without_indexing, 4,
        name_index);
    if (name_index == 0) {
        encode_hpack_string(out, name);
    }
    encode_hpack_string(out, value);
}

}  // namespace

void hpack_encoder::encode_indexed(std::pmr::string& out, std::uint32_t index) {
    if (index == 0) {
        throw std::invalid_argument("HPACK indexed fields require a nonzero index");
    }
    encode_hpack_integer(out, 0x80, 7, index);
}

void hpack_encoder::encode_dynamic_table_size_update(std::pmr::string& out, std::uint32_t maximum) {
    // RFC 7541 §6.3: 001xxxxx followed by an HPACK integer with a 5-bit prefix.
    encode_hpack_integer(out, 0x20, 5, maximum);
}

void hpack_encoder::encode_header(
    std::pmr::string& out, std::string_view name, std::string_view value) {
    // Validate both literals before static-table matching or writing the field
    // prefix. A string_view can be larger than HPACK's uint32 length domain, and
    // rejecting it here keeps the output unchanged when the input is invalid.
    validate_hpack_string_length(name);
    validate_hpack_string_length(value);
    const auto match = detail::hpack_static_fields.find(name, value);
    if (match && match->exact_index_) {
        // A fully indexed static-table entry carries no field value on the wire, so
        // there is nothing for an intermediary to index; the never-indexed hint does
        // not apply.
        encode_indexed(out, static_cast<std::uint32_t>(*match->exact_index_) + 1);
        return;
    }

    const bool never_indexed = detail::hpack_header_name_is_sensitive(name);
    if (match) {
        encode_header_with_name_index(out, static_cast<std::uint32_t>(match->name_index_) + 1, value,
            {.never_indexed_ = never_indexed});
        return;
    }
    const auto storage = std::string_view(out.data(), out.size() + 1);
    if (detail::http_text_overlaps(name, storage) || detail::http_text_overlaps(value, storage)) {
        std::pmr::string owned_name("", out.get_allocator());
        std::pmr::string owned_value("", out.get_allocator());
        if (detail::http_text_overlaps(name, storage)) {
            owned_name.assign(name);
            name = owned_name;
        }
        if (detail::http_text_overlaps(value, storage)) {
            owned_value.assign(value);
            value = owned_value;
        }
        encode_unindexed_literal(out, never_indexed, 0, name, value);
        return;
    }
    encode_unindexed_literal(out, never_indexed, 0, name, value);
}

void hpack_encoder::encode_header_with_name_index(std::pmr::string& out, std::uint32_t name_index,
    std::string_view value, hpack_header_with_name_index_options options) {
    if (name_index == 0) {
        throw std::invalid_argument("HPACK indexed names require a nonzero index");
    }
    validate_hpack_string_length(value);
    if (detail::http_text_overlaps(value, std::string_view(out.data(), out.size() + 1))) {
        const std::pmr::string owned_value(value, out.get_allocator());
        encode_unindexed_literal(out, options.never_indexed_, name_index, {}, owned_value);
        return;
    }
    encode_unindexed_literal(out, options.never_indexed_, name_index, {}, value);
}

void hpack_encoder::encode_status(std::pmr::string& out, http_status_code status) {
    switch (status.value()) {
        case http_status::ok.value():
            encode_indexed(out, detail::hpack_static_index::status_ok);
            return;
        case http_status::no_content.value():
            encode_indexed(out, detail::hpack_static_index::status_no_content);
            return;
        case http_status::partial_content.value():
            encode_indexed(out, detail::hpack_static_index::status_partial_content);
            return;
        case http_status::not_modified.value():
            encode_indexed(out, detail::hpack_static_index::status_not_modified);
            return;
        case http_status::bad_request.value():
            encode_indexed(out, detail::hpack_static_index::status_bad_request);
            return;
        case http_status::not_found.value():
            encode_indexed(out, detail::hpack_static_index::status_not_found);
            return;
        case http_status::internal_server_error.value():
            encode_indexed(out, detail::hpack_static_index::status_internal_server_error);
            return;
        default:
            break;
    }

    const auto token = detail::http_status_code_token(status);
    encode_header_with_name_index(
        out, detail::hpack_static_index::status_ok, detail::http_status_code_token_view(token));
}

}  // namespace ruvia
