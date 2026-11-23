#include "http2_hpack.h"

#include <nghttp2/nghttp2.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http_c_library_memory.h"
#include "http_field_kind.h"
#include "http_field_text.h"
#include "http_status.h"

namespace ruvia {
namespace detail {

class hpack_decoder final {
public:
    hpack_decoder() :
        memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)) {
        inflater_ = create_inflater();
    }
    ~hpack_decoder() {
        if (inflater_ != nullptr) {
            nghttp2_hd_inflate_del(inflater_);
        }
    }
    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;

    void reset() {
        if (decoding_) {
            throw std::logic_error("HPACK decoder cannot reset during decoding");
        }
        auto* replacement = create_inflater();
        nghttp2_hd_inflate_del(std::exchange(inflater_, replacement));
        poisoned_ = false;
        in_fragment_sequence_ = false;
        current_block_rejected_ = false;
    }

    void set_max_dynamic_table_size(std::uint32_t bytes) {
        if (decoding_ || in_fragment_sequence_) {
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

    [[nodiscard]] std::optional<hpack_decode_error> decode(byte_view fragment, bool end_headers, void* target, bool (*callback)(void*, std::string_view, std::string_view)) {
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

        in_fragment_sequence_ = !end_headers;
        const auto* cursor = reinterpret_cast<const std::uint8_t*>(fragment.data());
        auto remaining = fragment.size();
        for (;;) {
            nghttp2_nv field{};
            int flags = 0;
            const auto consumed = nghttp2_hd_inflate_hd3(inflater_, &field, &flags, cursor, remaining, end_headers ? 1 : 0);
            if (consumed < 0) {
                poisoned_ = true;
                in_fragment_sequence_ = false;
                current_block_rejected_ = false;
                if (consumed == NGHTTP2_ERR_NOMEM) {
                    memory_.rethrow_allocation_failure();
                }
                return hpack_decode_error::compression_error;
            }
            cursor += consumed;
            remaining -= static_cast<std::size_t>(consumed);
            if ((flags & NGHTTP2_HD_INFLATE_EMIT) != 0 && !current_block_rejected_ && !callback(target, std::string_view(reinterpret_cast<const char*>(field.name), field.namelen), std::string_view(reinterpret_cast<const char*>(field.value), field.valuelen))) {
                current_block_rejected_ = true;
            }
            if ((flags & NGHTTP2_HD_INFLATE_FINAL) != 0) {
                nghttp2_hd_inflate_end_headers(inflater_);
                const bool rejected = std::exchange(current_block_rejected_, false);
                in_fragment_sequence_ = false;
                return rejected ? std::optional(hpack_decode_error::callback_rejected) : std::nullopt;
            }
            if (!end_headers && remaining == 0 && (flags & NGHTTP2_HD_INFLATE_EMIT) == 0) {
                return current_block_rejected_ ? std::optional(hpack_decode_error::callback_rejected) : std::nullopt;
            }
        }
    }

private:
    [[nodiscard]] nghttp2_hd_inflater* create_inflater() {
        nghttp2_hd_inflater* inflater = nullptr;
        const int result = nghttp2_hd_inflate_new2(&inflater, &memory_table_);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::runtime_error("nghttp2 failed to create an HPACK inflater");
        }
        return inflater;
    }

    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    nghttp2_hd_inflater* inflater_{nullptr};
    bool decoding_{false};
    bool poisoned_{false};
    bool in_fragment_sequence_{false};
    bool current_block_rejected_{false};
};

class hpack_encoder final {
public:
    explicit hpack_encoder(std::uint32_t max_dynamic_table_bytes, bool suppress_initial_zero_update = false) :
        memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)),
        deflater_(create_deflater(max_dynamic_table_bytes, suppress_initial_zero_update)) {}

    ~hpack_encoder() {
        if (deflater_ != nullptr) {
            nghttp2_hd_deflate_del(deflater_);
        }
    }
    hpack_encoder(const hpack_encoder&) = delete;
    hpack_encoder& operator=(const hpack_encoder&) = delete;

    void reset(std::uint32_t max_dynamic_table_bytes) {
        auto* replacement = create_deflater(max_dynamic_table_bytes, false);
        nghttp2_hd_deflate_del(std::exchange(deflater_, replacement));
    }

    void set_max_dynamic_table_size(std::uint32_t bytes) {
        const int result = nghttp2_hd_deflate_change_table_size(deflater_, bytes);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::invalid_argument("invalid HPACK encoder dynamic table size");
        }
    }

    void encode_nv(std::string& output, const nghttp2_nv* fields, std::size_t count) {
        const auto bound = nghttp2_hd_deflate_bound(deflater_, fields, count);
        if (bound > output.max_size() - output.size()) {
            throw std::length_error("HPACK block exceeds the output size limit");
        }
        const auto previous_size = output.size();
        output.resize(previous_size + bound);
        const auto encoded = nghttp2_hd_deflate_hd2(deflater_, reinterpret_cast<std::uint8_t*>(output.data() + previous_size), bound, fields, count);
        if (encoded < 0) {
            output.resize(previous_size);
            if (encoded == NGHTTP2_ERR_NOMEM) {
                memory_.rethrow_allocation_failure();
            }
            throw std::length_error("nghttp2 rejected the HPACK field block");
        }
        output.resize(previous_size + static_cast<std::size_t>(encoded));
    }

    void encode_one(std::string& output, std::string_view name, std::string_view value, bool never_index) {
        std::string owned_name;
        std::string owned_value;
        if (http_text_aliases_string(name, output)) {
            owned_name.assign(name);
            name = owned_name;
        }
        if (http_text_aliases_string(value, output)) {
            owned_value.assign(value);
            value = owned_value;
        }
        const bool sensitive = never_index || http_field_never_indexed(name);
        const nghttp2_nv field{
            .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
            .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
            .namelen = name.size(),
            .valuelen = value.size(),
            .flags = static_cast<std::uint8_t>(sensitive ? NGHTTP2_NV_FLAG_NO_INDEX : NGHTTP2_NV_FLAG_NONE),
        };
        encode_nv(output, &field, 1);
    }

    void encode_block(std::string& output, std::span<const http_header_view> headers) {
        std::string aliased_storage;
        std::size_t aliased_bytes = 0;
        for (const auto& header : headers) {
            if (http_text_aliases_string(header.name(), output)) {
                aliased_bytes += header.name().size();
            }
            if (http_text_aliases_string(header.value(), output)) {
                aliased_bytes += header.value().size();
            }
        }
        if (aliased_bytes != 0) {
            aliased_storage.reserve(aliased_bytes);
        }
        std::vector<nghttp2_nv> fields;
        fields.reserve(headers.size());
        for (const auto& header : headers) {
            auto name = header.name();
            auto value = header.value();
            if (http_text_aliases_string(name, output)) {
                const auto offset = aliased_storage.size();
                aliased_storage.append(name);
                name = std::string_view(aliased_storage).substr(offset, name.size());
            }
            if (http_text_aliases_string(value, output)) {
                const auto offset = aliased_storage.size();
                aliased_storage.append(value);
                value = std::string_view(aliased_storage).substr(offset, value.size());
            }
            const bool sensitive = http_field_never_indexed(name);
            fields.push_back(nghttp2_nv{
                .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
                .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
                .namelen = name.size(),
                .valuelen = value.size(),
                .flags = static_cast<std::uint8_t>(sensitive ? NGHTTP2_NV_FLAG_NO_INDEX : NGHTTP2_NV_FLAG_NONE),
            });
        }
        encode_nv(output, fields.empty() ? nullptr : fields.data(), fields.size());
    }

private:
    [[nodiscard]] nghttp2_hd_deflater* create_deflater(std::uint32_t max_dynamic_table_bytes, bool suppress_initial_zero_update) {
        nghttp2_hd_deflater* deflater = nullptr;
        const int result = nghttp2_hd_deflate_new2(&deflater, max_dynamic_table_bytes, &memory_table_);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::runtime_error("nghttp2 failed to create an HPACK deflater");
        }
        if (suppress_initial_zero_update && max_dynamic_table_bytes < 4096) {
            std::array<std::uint8_t, 8> announcement{};
            if (nghttp2_hd_deflate_hd2(deflater, announcement.data(), announcement.size(), nullptr, 0) < 0) {
                nghttp2_hd_deflate_del(deflater);
                throw std::runtime_error("nghttp2 failed to prime the HPACK deflater");
            }
        }
        return deflater;
    }

    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    nghttp2_hd_deflater* deflater_{nullptr};
};

}  // namespace detail

hpack_decoder::hpack_decoder() :
    impl_(std::make_unique<detail::hpack_decoder>()) {}
hpack_decoder::~hpack_decoder() = default;
hpack_decoder::hpack_decoder(hpack_decoder&&) noexcept = default;
hpack_decoder& hpack_decoder::operator=(hpack_decoder&&) noexcept = default;

void hpack_decoder::reset() {
    impl_->reset();
}

void hpack_decoder::set_max_dynamic_table_size(hpack_table_size bytes_value) {
    impl_->set_max_dynamic_table_size(bytes_value.value());
}

std::optional<hpack_decode_error> hpack_decoder::decode_with_callback(byte_view fragment, bool end_headers, void* target, header_callback_type callback_value) {
    return impl_->decode(fragment, end_headers, target, callback_value);
}

hpack_encoder::hpack_encoder(hpack_table_size max_dynamic_table_bytes) :
    impl_(std::make_unique<detail::hpack_encoder>(max_dynamic_table_bytes.value(), max_dynamic_table_bytes.value() == 0)) {}
hpack_encoder::~hpack_encoder() = default;
hpack_encoder::hpack_encoder(hpack_encoder&&) noexcept = default;
hpack_encoder& hpack_encoder::operator=(hpack_encoder&&) noexcept = default;

void hpack_encoder::reset(hpack_table_size max_dynamic_table_bytes) {
    impl_->reset(max_dynamic_table_bytes.value());
}

void hpack_encoder::set_max_dynamic_table_size(hpack_table_size bytes) {
    impl_->set_max_dynamic_table_size(bytes.value());
}

void hpack_encoder::encode_field(std::string& output, std::string_view name, std::string_view value, bool never_index) {
    impl_->encode_one(output, name, value, never_index);
}

void hpack_encoder::encode_headers(std::string& output, std::span<const http_header_view> headers) {
    impl_->encode_block(output, headers);
}

void hpack_encoder::encode_status_field(std::string& output, http_status_code status) {
    const auto token = detail::http_status_code_token(status);
    impl_->encode_one(output, ":status", detail::http_status_code_token_view(token), false);
}

void hpack_encoder::encode_header(std::string& output, std::string_view name, std::string_view value) {
    detail::hpack_encoder deflater(0, true);
    deflater.encode_one(output, name, value, false);
}

void hpack_encoder::encode_status(std::string& output, http_status_code status) {
    const auto token = detail::http_status_code_token(status);
    encode_header(output, ":status", detail::http_status_code_token_view(token));
}

}  // namespace ruvia
