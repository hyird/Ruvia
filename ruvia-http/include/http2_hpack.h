#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "byte_view.h"
#include "checked_integer.h"
#include "http_header.h"
#include "http_status.h"

namespace ruvia {

namespace detail {
class hpack_decoder;
class hpack_encoder;

struct hpack_table_size_policy final {
    static constexpr std::uint32_t default_value = 0;
    static constexpr bool implicit_conversion = true;
    static constexpr const char* invalid_value_message = "HPACK dynamic table size must be in 0..4294967295";

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::in_range<std::uint32_t>(+value);
    }
};
}  // namespace detail

// 32-bit HPACK dynamic-table byte capacity (RFC 7541 section 6.3). Checked
// before narrowing so negative or wider integers cannot wrap into valid sizes.
class hpack_table_size final : public checked_integer<hpack_table_size, std::uint32_t, detail::hpack_table_size_policy> {
public:
    using checked_integer::checked_integer;
};

static_assert(std::is_trivially_copyable_v<hpack_table_size>);
static_assert(sizeof(hpack_table_size) == sizeof(std::uint32_t));

// nghttp2's inflater reports every malformed or truncated block as one
// decompression failure (RFC 7541 section 3.2 makes any of them a connection
// error), so the cause is not subdivided.
enum class hpack_decode_error : std::uint8_t {
    compression_error,
    callback_rejected,
};

// Stateful RFC 7541 decoder backed by nghttp2's HPACK inflater.
// Supports both complete blocks (`decode(block, cb)`) and CONTINUATION
// fragments (`decode(fragment, end_headers, cb)`), plus `reset()` for reuse.
// Callback rejection stops further callbacks for the current header block while
// still decoding the remaining bytes so the dynamic table stays synchronized.
// A compression error or allocation failure poisons the decoder until `reset()`.
class hpack_decoder final {
public:
    hpack_decoder();
    ~hpack_decoder();
    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;
    hpack_decoder(hpack_decoder&&) noexcept;
    hpack_decoder& operator=(hpack_decoder&&) noexcept;

    // Resets dynamic-table and fragment state to a fresh RFC 7541 decoder.
    // Throws std::logic_error if invoked inside a decode() callback.
    void reset();

    // Only between complete header blocks. During a callback or mid-fragment
    // sequence this throws std::logic_error before mutating state.
    void set_max_dynamic_table_size(hpack_table_size bytes);

    template <typename callback>
        requires std::predicate<callback&, std::string_view, std::string_view>
    [[nodiscard]] std::optional<hpack_decode_error> decode(byte_view fragment, bool end_headers, callback&& callback_value) {
        std::exception_ptr callback_failure;
        auto invocation = [&callback_value, &callback_failure](std::string_view name, std::string_view value) {
            if (callback_failure) {
                return true;
            }
            try {
                return static_cast<bool>(std::invoke(callback_value, name, value));
            } catch (...) {
                callback_failure = std::current_exception();
                return true;
            }
        };
        using invocation_type = decltype(invocation);
        try {
            auto result_value = decode_with_callback(fragment, end_headers, std::addressof(invocation), [](void* target, std::string_view name, std::string_view value) {
                return std::invoke(*static_cast<invocation_type*>(target), name, value);
            });
            if (callback_failure) {
                std::rethrow_exception(callback_failure);
            }
            return result_value;
        } catch (...) {
            if (callback_failure) {
                std::rethrow_exception(callback_failure);
            }
            throw;
        }
    }

    template <typename callback>
        requires std::predicate<callback&, std::string_view, std::string_view>
    [[nodiscard]] std::optional<hpack_decode_error> decode(std::string_view fragment, bool end_headers, callback&& callback_value) {
        return decode(as_bytes(fragment), end_headers, std::forward<callback>(callback_value));
    }

    template <typename bytes_type, typename callback>
        requires(!std::convertible_to<bytes_type&&, std::string_view> && std::convertible_to<bytes_type&&, byte_view> && std::predicate<callback&, std::string_view, std::string_view>)
    [[nodiscard]] std::optional<hpack_decode_error> decode(bytes_type&& block, callback&& callback_value) {
        return decode(byte_view(std::forward<bytes_type>(block)), true, std::forward<callback>(callback_value));
    }

    template <typename callback>
        requires std::predicate<callback&, std::string_view, std::string_view>
    [[nodiscard]] std::optional<hpack_decode_error> decode(std::string_view block, callback&& callback_value) {
        return decode(as_bytes(block), true, std::forward<callback>(callback_value));
    }

private:
    using header_callback_type = bool (*)(void*, std::string_view, std::string_view);
    [[nodiscard]] std::optional<hpack_decode_error> decode_with_callback(byte_view fragment, bool end_headers, void* target, header_callback_type callback);

    std::unique_ptr<detail::hpack_decoder> impl_;
};

// RFC 7541 HPACK encoder backed by nghttp2's deflater.
// Supports both stateful connection-scoped encoding (with configurable dynamic
// table size) and stateless single-field static helpers. Sensitive fields
// (authorization, proxy-authorization, cookie, set-cookie) are always marked
// never-indexed (RFC 7541 section 7.1.3).
class hpack_encoder final {
public:
    explicit hpack_encoder(hpack_table_size max_dynamic_table_bytes = hpack_table_size{0});
    ~hpack_encoder();
    hpack_encoder(const hpack_encoder&) = delete;
    hpack_encoder& operator=(const hpack_encoder&) = delete;
    hpack_encoder(hpack_encoder&&) noexcept;
    hpack_encoder& operator=(hpack_encoder&&) noexcept;

    void reset(hpack_table_size max_dynamic_table_bytes = hpack_table_size{0});
    void set_max_dynamic_table_size(hpack_table_size bytes);

    void encode_field(std::string& output, std::string_view name, std::string_view value, bool never_index = false);
    void encode_headers(std::string& output, std::span<const http_header_view> headers);
    void encode_status_field(std::string& output, http_status_code status);

    // Stateless zero-dynamic-table helpers: every encoded field is self-contained.
    static void encode_header(std::string& output, std::string_view name, std::string_view value);
    static void encode_status(std::string& output, http_status_code status);

private:
    std::unique_ptr<detail::hpack_encoder> impl_;
};

}  // namespace ruvia
