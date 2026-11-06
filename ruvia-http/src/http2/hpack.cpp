#include "ruvia/http/hpack.h"

#include <nghttp2/nghttp2.h>

#include <array>
#include <limits>
#include <new>
#include <stdexcept>

#include "ruvia/http/detail/util/http_address_overlap.h"
#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"

#include "http2/http2_header_rules.h"
#include "http2/http2_nghttp2_memory.h"

namespace ruvia {
namespace detail {

class hpack_decoder final {
public:
    explicit hpack_decoder(std::pmr::memory_resource* resource)
        : memory_(resource) {
        const int result = nghttp2_hd_inflate_new2(&inflater_, memory_.table());
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

    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return memory_.resource();
    }

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

    [[nodiscard]] std::optional<hpack_decode_error> decode(
        std::string_view block, void* target, bool (*callback)(void*, std::string_view, std::string_view)) {
        if (decoding_) {
            throw std::logic_error("nested HPACK decode on the same decoder");
        }
        if (poisoned_) {
            return hpack_decode_error::compression_error;
        }
        struct decoding_guard final {
            bool& decoding_;
            explicit decoding_guard(bool& decoding) noexcept
                : decoding_(decoding) {
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
            if ((flags & NGHTTP2_HD_INFLATE_EMIT) != 0 && !rejected &&
                !callback(target, std::string_view(reinterpret_cast<const char*>(field.name), field.namelen),
                    std::string_view(reinterpret_cast<const char*>(field.value), field.valuelen))) {
                rejected = true;
            }
            if ((flags & NGHTTP2_HD_INFLATE_FINAL) != 0) {
                nghttp2_hd_inflate_end_headers(inflater_);
                break;
            }
            if ((flags & NGHTTP2_HD_INFLATE_EMIT) == 0 && remaining == 0) {
                break;
            }
        }
        return rejected ? std::optional(hpack_decode_error::callback_rejected) : std::nullopt;
    }

private:
    http2_nghttp2_memory memory_;
    nghttp2_hd_inflater* inflater_{nullptr};
    bool decoding_{false};
    bool poisoned_{false};
};

namespace {

// One per thread: with a zero-capacity table the deflater carries no state
// between fields once its initial capacity notification has been consumed.
class hpack_field_deflater final {
public:
    hpack_field_deflater() {
        const int result = nghttp2_hd_deflate_new(&deflater_, 0);
        if (result == NGHTTP2_ERR_NOMEM) {
            throw std::bad_alloc();
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

    void encode(std::pmr::string& output, std::string_view name, std::string_view value, bool sensitive) {
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
        const auto encoded = nghttp2_hd_deflate_hd2(deflater_,
            reinterpret_cast<std::uint8_t*>(output.data() + previous_size), bound, &field, 1);
        if (encoded < 0) {
            output.resize(previous_size);
            if (encoded == NGHTTP2_ERR_NOMEM) {
                throw std::bad_alloc();
            }
            throw std::length_error("nghttp2 rejected the HPACK field");
        }
        output.resize(previous_size + static_cast<std::size_t>(encoded));
    }

private:
    nghttp2_hd_deflater* deflater_{nullptr};
};

[[nodiscard]] hpack_field_deflater& thread_field_deflater() {
    thread_local hpack_field_deflater deflater;
    return deflater;
}

}  // namespace
}  // namespace detail

void hpack_decoder::impl_deleter_type::operator()(detail::hpack_decoder* value) const noexcept {
    if (value != nullptr) {
        detail::destroy_http_pmr_object(detail::http_resolved_pmr_resource_tag{}, value, value->resource());
    }
}

hpack_decoder::hpack_decoder(hpack_decoder_options options) {
    auto* resource = detail::http_pmr_resource_or_default(options.resource_);
    impl_.reset(detail::construct_http_pmr_object<detail::hpack_decoder>(
        detail::http_resolved_pmr_resource_tag{}, resource, resource));
}
hpack_decoder::~hpack_decoder() = default;
hpack_decoder::hpack_decoder(hpack_decoder&&) noexcept = default;
hpack_decoder& hpack_decoder::operator=(hpack_decoder&&) noexcept = default;

void hpack_decoder::set_max_dynamic_table_size(hpack_table_size bytes_value) {
    impl_->set_max_dynamic_table_size(bytes_value.value());
}

std::optional<hpack_decode_error> hpack_decoder::decode_with_callback(
    std::string_view block, void* target, header_callback_type callback_value) {
    return impl_->decode(block, target, callback_value);
}

void hpack_encoder::encode_header(std::pmr::string& output, std::string_view name, std::string_view value) {
    // Resizing output may invalidate borrowed views; own them in output's resource first.
    std::pmr::string owned_name(output.get_allocator());
    std::pmr::string owned_value(output.get_allocator());
    if (detail::http_text_aliases_string(name, output)) {
        owned_name.assign(name);
        name = owned_name;
    }
    if (detail::http_text_aliases_string(value, output)) {
        owned_value.assign(value);
        value = owned_value;
    }
    detail::thread_field_deflater().encode(
        output, name, value, detail::hpack_header_name_is_sensitive(name));
}

void hpack_encoder::encode_status(std::pmr::string& output, http_status_code status) {
    const auto token = detail::http_status_code_token(status);
    encode_header(output, ":status", detail::http_status_code_token_view(token));
}

}  // namespace ruvia
