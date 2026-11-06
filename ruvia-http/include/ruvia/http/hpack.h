#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/http_status.h"

namespace ruvia {

namespace detail {
class hpack_decoder;
}  // namespace detail

// 32-bit HPACK dynamic-table byte capacity (RFC 7541 section 6.3). Checked
// before narrowing so negative or wider integers cannot wrap into valid sizes.
class hpack_table_size final {
public:
    constexpr hpack_table_size() noexcept = default;

    template <std::integral integer_type>
    constexpr hpack_table_size(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint32_t>(value)
                     : throw std::invalid_argument("HPACK dynamic table size must be in 0..4294967295")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::in_range<std::uint32_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<hpack_table_size> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return hpack_table_size(static_cast<std::uint32_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(hpack_table_size, hpack_table_size) = default;

private:
    struct validated_tag_type {};

    constexpr hpack_table_size(std::uint32_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint32_t value_{0};
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

struct hpack_decoder_options final {
    // Must outlive the decoder, including its implementation and dynamic table.
    // nullptr binds the current default PMR resource at construction.
    std::pmr::memory_resource* resource_{nullptr};
};

// Stateful RFC 7541 decoder backed by nghttp2's HPACK inflater; every
// allocation, the dynamic table included, uses the configured resource.
// decode() consumes one complete field block. Callback rejection stops further
// callbacks but still decodes the rest of the block, so the dynamic table stays
// synchronized and the decoder remains usable. A compression error or an
// allocation failure leaves the table out of step with the peer (RFC 7541 section
// 2.2 offers no recovery): the decoder is poisoned, every later decode() reports
// compression_error, and it must be discarded. Leading dynamic-table capacity
// updates are applied in wire order; updates after a field or above the
// configured maximum are compression errors.
// Lowering the maximum below the table's current capacity evicts at once, and
// the next block must then begin with a capacity update no larger than the
// lowest maximum set since (RFC 7541 section 4.2); a block without one is a
// compression error. Raising the maximum requires no update.
class hpack_decoder final {
public:
    explicit hpack_decoder(hpack_decoder_options options = {});
    ~hpack_decoder();
    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;
    hpack_decoder(hpack_decoder&&) noexcept;
    hpack_decoder& operator=(hpack_decoder&&) noexcept;

    // Only between decode() calls. During a header callback this throws
    // std::logic_error before changing either the allowance or the table.
    void set_max_dynamic_table_size(hpack_table_size bytes);

    // Header name/value views are borrowed only for the callback invocation.
    // Copy into owned storage when retaining a field beyond that invocation.
    // A callback must keep this decoder's implementation alive until decode()
    // returns. Nested decode() on the same implementation throws std::logic_error;
    // a different decoder may be used. Callback exceptions are rethrown after
    // consuming the block, so a valid block still synchronizes the dynamic table.
    template <typename callback>
        requires std::predicate<callback&, std::string_view, std::string_view>
    [[nodiscard]] std::optional<hpack_decode_error> decode(std::string_view block, callback&& callback_value) {
        // Always pass an object through the erased callback boundary. Taking
        // addressof(callback) directly would produce a function pointer when a
        // free function is supplied, which cannot be represented by void*.
        std::exception_ptr callback_failure;
        auto invocation = [&callback_value, &callback_failure](
                              std::string_view name, std::string_view value) {
            if (callback_failure) {
                return true;
            }
            try {
                return static_cast<bool>(std::invoke(callback_value, name, value));
            } catch (...) {
                callback_failure = std::current_exception();
                // Keep consuming the block so dynamic-table state remains
                // synchronized before the user's exception is rethrown.
                return true;
            }
        };
        using invocation_type = decltype(invocation);
        try {
            auto result_value = decode_with_callback(block, std::addressof(invocation),
                [](void* target, std::string_view name, std::string_view value) {
                    return std::invoke(*static_cast<invocation_type*>(target), name, value);
                });
            if (callback_failure) {
                std::rethrow_exception(callback_failure);
            }
            return result_value;
        } catch (...) {
            // A callback exception is the initiating failure; it still reaches
            // its owner when continuing the block failed as well.
            if (callback_failure) {
                std::rethrow_exception(callback_failure);
            }
            throw;
        }
    }

private:
    using header_callback_type = bool (*)(void*, std::string_view, std::string_view);
    [[nodiscard]] std::optional<hpack_decode_error> decode_with_callback(
        std::string_view block, void* target, header_callback_type callback);

    struct impl_deleter_type final {
        void operator()(detail::hpack_decoder* value) const noexcept;
    };
    std::unique_ptr<detail::hpack_decoder, impl_deleter_type> impl_;
};

// Field-level HPACK encoding through nghttp2's deflater with a zero-capacity
// dynamic table, so no call depends on or changes encoder state: an exact
// static-table match is indexed, any other field is a literal without indexing
// (never indexed for authorization, proxy-authorization, cookie and
// set-cookie, RFC 7541 section 7.1.3), Huffman-coded when that is shorter.
class hpack_encoder final {
public:
    // Names and values may borrow output. Aliasing inputs are owned in output's
    // resource before appending can invalidate them. Allocation failure leaves
    // the existing output bytes unchanged.
    static void encode_header(
        std::pmr::string& output, std::string_view name, std::string_view value);
    static void encode_status(std::pmr::string& output, http_status_code status);
};

}  // namespace ruvia
