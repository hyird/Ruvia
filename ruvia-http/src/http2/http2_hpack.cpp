#include "http2/http2_hpack.h"

#include <algorithm>
#include <exception>
#include <variant>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "field/hpack_huffman.h"
#include "util/pmr_string.h"

namespace ruvia::detail {

hpack_decoder::hpack_decoder(hpack_decoder_options options)
    : resource_(http_pmr_resource_or_default(options.resource_)),
      dynamic_(std::size_t{0}, resource_),
      name_scratch_(std::string_view{}, resource_),
      value_scratch_(std::string_view{}, resource_) {}

hpack_decoder::decode_transaction_type::decode_transaction_type(hpack_decoder& decoder) noexcept
    : decoder_(&decoder) {
    decoder_->begin_decode_transaction();
}

hpack_decoder::decode_transaction_type::~decode_transaction_type() {
    rollback();
}

void hpack_decoder::decode_transaction_type::commit() noexcept {
    if (!active_) {
        return;
    }
    decoder_->commit_decode_transaction();
    active_ = false;
}

void hpack_decoder::decode_transaction_type::rollback() noexcept {
    if (!active_) {
        return;
    }
    decoder_->rollback_decode_transaction();
    active_ = false;
}

void hpack_decoder::set_max_dynamic_table_size(std::size_t bytes_value) {
    allowed_dynamic_size_ = bytes_value;
    max_dynamic_size_ = std::min(max_dynamic_size_, bytes_value);
    evict_dynamic();
}

hpack_decoder::decode_transaction_type hpack_decoder::begin_transaction() noexcept {
    return decode_transaction_type(*this);
}

hpack_decoder::step_result_type hpack_decoder::decode_integer(const unsigned char*& cursor_value,
    const unsigned char* end, std::uint8_t prefix_bits, std::uint32_t& value) const noexcept {
    if (cursor_value == end || prefix_bits == 0 || prefix_bits > 8) {
        return hpack_decode_error::need_more;
    }

    const auto prefix_mask = static_cast<std::uint32_t>((1U << prefix_bits) - 1U);
    value = static_cast<std::uint32_t>(*cursor_value++ & prefix_mask);
    if (value < prefix_mask) {
        return std::nullopt;
    }

    std::uint32_t shift = 0;
    for (;;) {
        if (cursor_value == end) {
            return hpack_decode_error::need_more;
        }
        const auto byte = static_cast<std::uint32_t>(*cursor_value++);
        // Bound the chunk so the shift itself stays within uint32...
        if (shift >= 28 && (byte & 0x7fU) > 0x0fU) {
            return hpack_decode_error::integer_overflow;
        }
        // ...then reject the case where the accumulated sum would still overflow
        // uint32 (e.g. FF FF FF FF FF 0F): the chunk guard alone permits 0x0f<<28,
        // which added to a maxed lower value wraps past UINT32_MAX (RFC 7541 5.1).
        const auto addend = (byte & 0x7fU) << shift;
        if (value > 0xFFFFFFFFU - addend) {
            return hpack_decode_error::integer_overflow;
        }
        value += addend;
        if ((byte & 0x80U) == 0) {
            return std::nullopt;
        }
        shift += 7;
        if (shift > 28) {
            return hpack_decode_error::integer_overflow;
        }
    }
}

hpack_decoder::step_result_type hpack_decoder::decode_string(const unsigned char*& cursor_value,
    const unsigned char* end, std::pmr::string& scratch, std::string_view& value) {
    if (cursor_value == end) {
        return hpack_decode_error::need_more;
    }
    const bool huffman = (*cursor_value & 0x80U) != 0;
    std::uint32_t size = 0;
    if (const auto error = decode_integer(cursor_value, end, 7, size); (error.has_value())) {
        return error;
    }
    if (static_cast<std::size_t>(end - cursor_value) < size) {
        return hpack_decode_error::need_more;
    }

    const std::string_view encoded(reinterpret_cast<const char*>(cursor_value), size);
    cursor_value += size;
    if (!huffman || encoded.empty()) {
        value = encoded;
        return std::nullopt;
    }

    scratch.clear();
    scratch.reserve(encoded.size());
    if (!append_hpack_huffman(encoded, scratch)) {
        return hpack_decode_error::invalid_huffman;
    }
    value = scratch;
    return std::nullopt;
}

hpack_decoder::step_result_type hpack_decoder::decode_literal_header(const unsigned char*& cursor_value,
    const unsigned char* end, std::uint8_t name_index_prefix_bits, bool index_into_dynamic, void* target,
    header_callback_type callback_value, bool& rejected) {
    std::uint32_t name_index = 0;
    if (const auto error = decode_integer(cursor_value, end, name_index_prefix_bits, name_index);
        (error.has_value())) {
        return error;
    }

    std::string_view name;
    if (name_index == 0) {
        if (const auto error = decode_string(cursor_value, end, name_scratch_, name); (error.has_value())) {
            return error;
        }
    } else if (const auto error = indexed_name(name_index, name); (error.has_value())) {
        return error;
    }

    std::string_view value;
    if (const auto error = decode_string(cursor_value, end, value_scratch_, value); (error.has_value())) {
        return error;
    }
    // Suppress the callback once rejected, but ALWAYS apply the dynamic-table insertion
    // below -- skipping it would desync the connection-global table for every later
    // block (RFC 7541 §4.1). The rejection is reported after the whole block decodes.
    if (!rejected && callback_value != nullptr && !callback_value(target, name, value)) {
        rejected = true;
    }
    if (index_into_dynamic) {
        add_dynamic(name, value);
    }
    return std::nullopt;
}

void hpack_decoder::release_scratch() {
    clear_pmr_string_retaining_small(name_scratch_);
    clear_pmr_string_retaining_small(value_scratch_);
}

hpack_decode_result hpack_decoder::decode(
    std::string_view block, void* target, header_callback_type callback_value) {
    auto transaction = begin_transaction();
    auto result_value = decode(block, target, callback_value, transaction);
    if (transaction.active()) {
        transaction.commit();
    }
    return result_value;
}

hpack_decode_result hpack_decoder::decode(
    std::string_view block, void* target, header_callback_type callback_value, decode_transaction_type& transaction) {
    if (transaction.decoder_ != this || !transaction.active_) {
        std::terminate();
    }
    try {
        auto result_value = decode_block(block, target, callback_value);
        if (const auto error = result_value.error();
            error.has_value() && *error != hpack_decode_error::callback_rejected) {
            transaction.rollback();
        }
        return result_value;
    } catch (...) {
        // A field block is the HPACK transaction boundary. In particular, a
        // later allocation failure must not leave earlier incremental-indexing
        // entries visible when the caller retries the same block.
        transaction.rollback();
        throw;
    }
}

hpack_decode_result hpack_decoder::decode_block(
    std::string_view block, void* target, header_callback_type callback_value) {
    struct scratch_release_guard final {
        hpack_decoder& decoder_;

        ~scratch_release_guard() {
            decoder_.release_scratch();
        }
    };

    scratch_release_guard scratch_guard{*this};
    const auto* cursor_value = reinterpret_cast<const unsigned char*>(block.data());
    const auto* const end = cursor_value + block.size();
    bool saw_header = false;
    std::uint8_t size_update_count = 0;
    std::uint32_t first_size_update = 0;
    // Callback rejection does NOT abort the block: the whole field block must be
    // decoded so the connection-global dynamic table stays consistent (RFC 7541 §4.1 /
    // RFC 9113 §4.3). We keep going with the callback suppressed and report the
    // rejection at the end; the caller then RST_STREAMs the (validated-as-bad) stream
    // while the connection -- and every later block on it -- decodes correctly.
    bool rejected = false;

    while (cursor_value < end) {
        const auto first = *cursor_value;
        if ((first & 0x80U) != 0) {
            std::uint32_t index = 0;
            if (const auto error = decode_integer(cursor_value, end, 7, index); (error.has_value())) {
                return hpack_decode_result(*error);
            }
            header_view_type header;
            if (const auto error = indexed_header(index, header); (error.has_value())) {
                return hpack_decode_result(*error);
            }
            if (!rejected && callback_value != nullptr && !callback_value(target, header.name_, header.value_)) {
                rejected = true;
            }
            saw_header = true;
            continue;
        }

        if ((first & 0x40U) != 0) {
            if (const auto error =
                    decode_literal_header(cursor_value, end, 6, true, target, callback_value, rejected);
                (error.has_value())) {
                return hpack_decode_result(*error);
            }
            saw_header = true;
            continue;
        }

        if ((first & 0xe0U) == 0x20U) {
            if (saw_header || size_update_count == 2) {
                return hpack_decode_result(hpack_decode_error::dynamic_table_size);
            }
            std::uint32_t size = 0;
            if (const auto error = decode_integer(cursor_value, end, 5, size); (error.has_value())) {
                return hpack_decode_result(*error);
            }
            // RFC 7541 section 4.2 permits at most two updates at the start
            // of a field block. When two are present, the first is the
            // smallest intervening maximum and the second is the final
            // maximum, so the second value cannot be lower than the first.
            if (size > allowed_dynamic_size_ || (size_update_count == 1 && size < first_size_update)) {
                return hpack_decode_result(hpack_decode_error::dynamic_table_size);
            }
            if (size_update_count == 0) {
                first_size_update = size;
            }
            ++size_update_count;
            max_dynamic_size_ = size;
            evict_dynamic();
            continue;
        }

        if ((first & 0xf0U) == 0x00U || (first & 0xf0U) == 0x10U) {
            if (const auto error =
                    decode_literal_header(cursor_value, end, 4, false, target, callback_value, rejected);
                (error.has_value())) {
                return hpack_decode_result(*error);
            }
            saw_header = true;
            continue;
        }

        return hpack_decode_result(hpack_decode_error::invalid_index);
    }

    // The whole block decoded and the dynamic table is consistent; surface a late
    // callback rejection now so the owner RST_STREAMs without desyncing the connection.
    return rejected ? hpack_decode_result(hpack_decode_error::callback_rejected) : hpack_decode_result();
}

}  // namespace ruvia::detail
