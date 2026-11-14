#include <nghttp2/nghttp2.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "http2_connection.h"
#include "http_c_library_memory.h"
#include "http_connection_fields.h"

namespace ruvia {
namespace detail {

class hpack_decoder final {
public:
    hpack_decoder() :
        memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)) {
        const int result = nghttp2_hd_inflate_new2(&inflater_, &memory_table_);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::runtime_error("nghttp2 failed to create an HPACK inflater");
        }
    }
    ~hpack_decoder() {
        nghttp2_hd_inflate_del(inflater_);
    }
    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;

    void set_max_dynamic_table_size(std::uint32_t bytes) {
        if (decoding_) {
            throw std::logic_error("HPACK table size cannot change during decoding");
        }
        const int result = nghttp2_hd_inflate_change_table_size(inflater_, bytes);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::invalid_argument("invalid HPACK dynamic table size");
        }
    }

    [[nodiscard]] std::optional<hpack_decode_error> decode(std::string_view block, void* target, bool (*callback)(void*, std::string_view, std::string_view)) {
        if (decoding_) {
            throw std::logic_error("nested HPACK decode on the same decoder");
        }
        if (poisoned_) {
            return hpack_decode_error::compression_error;
        }
        struct decoding_guard final {
            bool& decoding_;
            explicit decoding_guard(bool& decoding) noexcept :
                decoding_(decoding) {
                decoding_ = true;
            }
            ~decoding_guard() {
                decoding_ = false;
            }
            decoding_guard(const decoding_guard&) = delete;
            decoding_guard& operator=(const decoding_guard&) = delete;
        } guard(decoding_);

        const auto* cursor = reinterpret_cast<const std::uint8_t*>(block.data());
        auto remaining = block.size();
        bool rejected = false;
        for (;;) {
            nghttp2_nv field{};
            int flags = 0;
            const auto consumed = nghttp2_hd_inflate_hd3(inflater_, &field, &flags, cursor, remaining, 1);
            if (consumed < 0) {
                // The table may already hold part of this block; it can no
                // longer be trusted to mirror the peer's encoder.
                poisoned_ = true;
                if (consumed == NGHTTP2_ERR_NOMEM) {
                    memory_.rethrow_allocation_failure();
                }
                return hpack_decode_error::compression_error;
            }
            cursor += consumed;
            remaining -= static_cast<std::size_t>(consumed);
            if ((flags & NGHTTP2_HD_INFLATE_EMIT) != 0 && !rejected && !callback(target, std::string_view(reinterpret_cast<const char*>(field.name), field.namelen), std::string_view(reinterpret_cast<const char*>(field.value), field.valuelen))) {
                rejected = true;
            }
            if ((flags & NGHTTP2_HD_INFLATE_FINAL) != 0) {
                nghttp2_hd_inflate_end_headers(inflater_);
                break;
            }
        }
        return rejected ? std::optional(hpack_decode_error::callback_rejected) : std::nullopt;
    }

private:
    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    nghttp2_hd_inflater* inflater_{nullptr};
    bool decoding_{false};
    bool poisoned_{false};
};

namespace {

// Uses a per-call deflater so encoding maintains no shared state.
class hpack_field_deflater final {
public:
    hpack_field_deflater() :
        memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)) {
        const int result = nghttp2_hd_deflate_new2(&deflater_, 0, &memory_table_);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::runtime_error("nghttp2 failed to create an HPACK deflater");
        }
        // A deflater whose table is smaller than RFC 7541's initial 4096 bytes
        // announces the reduction in its first block. Consume that update
        // here so every encoded field is self-contained.
        std::array<std::uint8_t, 8> announcement{};
        if (nghttp2_hd_deflate_hd2(deflater_, announcement.data(), announcement.size(), nullptr, 0) < 0) {
            nghttp2_hd_deflate_del(deflater_);
            throw std::runtime_error("nghttp2 failed to prime the HPACK deflater");
        }
    }
    ~hpack_field_deflater() {
        nghttp2_hd_deflate_del(deflater_);
    }
    hpack_field_deflater(const hpack_field_deflater&) = delete;
    hpack_field_deflater& operator=(const hpack_field_deflater&) = delete;

    void encode(std::string& output, std::string_view name, std::string_view value, bool sensitive) {
        nghttp2_nv field{
            .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
            .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
            .namelen = name.size(),
            .valuelen = value.size(),
            .flags = static_cast<std::uint8_t>(sensitive ? NGHTTP2_NV_FLAG_NO_INDEX : NGHTTP2_NV_FLAG_NONE),
        };
        const auto bound = nghttp2_hd_deflate_bound(deflater_, &field, 1);
        if (bound > output.max_size() - output.size()) {
            throw std::length_error("HPACK field exceeds the output size limit");
        }
        const auto previous_size = output.size();
        output.resize(previous_size + bound);
        const auto encoded = nghttp2_hd_deflate_hd2(deflater_, reinterpret_cast<std::uint8_t*>(output.data() + previous_size), bound, &field, 1);
        if (encoded < 0) {
            output.resize(previous_size);
            if (encoded == NGHTTP2_ERR_NOMEM) {
                memory_.rethrow_allocation_failure();
            }
            throw std::length_error("nghttp2 rejected the HPACK field");
        }
        output.resize(previous_size + static_cast<std::size_t>(encoded));
    }

private:
    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    nghttp2_hd_deflater* deflater_{nullptr};
};
}  // namespace
}  // namespace detail

hpack_decoder::hpack_decoder() :
    impl_(std::make_unique<detail::hpack_decoder>()) {}
hpack_decoder::~hpack_decoder() = default;
hpack_decoder::hpack_decoder(hpack_decoder&&) noexcept = default;
hpack_decoder& hpack_decoder::operator=(hpack_decoder&&) noexcept = default;

void hpack_decoder::set_max_dynamic_table_size(hpack_table_size bytes_value) {
    impl_->set_max_dynamic_table_size(bytes_value.value());
}

std::optional<hpack_decode_error> hpack_decoder::decode_with_callback(std::string_view block, void* target, header_callback_type callback_value) {
    return impl_->decode(block, target, callback_value);
}

void hpack_encoder::encode_header(std::string& output, std::string_view name, std::string_view value) {
    // Resizing output may invalidate borrowed views; own them first.
    std::string owned_name;
    std::string owned_value;
    if (detail::http_text_aliases_string(name, output)) {
        owned_name.assign(name);
        name = owned_name;
    }
    if (detail::http_text_aliases_string(value, output)) {
        owned_value.assign(value);
        value = owned_value;
    }
    detail::hpack_field_deflater deflater;
    deflater.encode(output, name, value, detail::http_field_never_indexed(name));
}

void hpack_encoder::encode_status(std::string& output, http_status_code status) {
    const auto token = detail::http_status_code_token(status);
    encode_header(output, ":status", detail::http_status_code_token_view(token));
}

}  // namespace ruvia