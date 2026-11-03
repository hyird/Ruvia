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

// Non-zero 32-bit HPACK table index (RFC 7541 section 2.3.3). Checked before
// narrowing so zero, negative, or wider integers cannot wrap into valid indices.
class hpack_index final {
public:
    template <std::integral integer_type>
    constexpr hpack_index(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint32_t>(value)
                     : throw std::invalid_argument("HPACK index must be in 1..4294967295")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::cmp_greater(+value, 0) && std::in_range<std::uint32_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<hpack_index> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return hpack_index(static_cast<std::uint32_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(hpack_index, hpack_index) = default;

private:
    struct validated_tag_type {};

    constexpr hpack_index(std::uint32_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint32_t value_{1};
};

static_assert(std::is_trivially_copyable_v<hpack_index>);
static_assert(sizeof(hpack_index) == sizeof(std::uint32_t));

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

enum class hpack_decode_error : std::uint8_t {
    need_more,
    integer_overflow,
    invalid_index,
    invalid_string,
    invalid_huffman,
    dynamic_table_size,
    callback_rejected,
};

struct hpack_decoder_options final {
    // Must outlive the decoder, including its implementation and dynamic table.
    // nullptr binds the current default PMR resource at construction.
    std::pmr::memory_resource* resource_{nullptr};
};

// Stateful RFC 7541 decoder. One decode() call is one dynamic-table
// transaction; protocol failures roll back table changes while callback
// rejection still consumes the complete block to keep peer state synchronized.
// A block retains its starting table snapshot for rollback, but entries inserted
// and evicted within that same block are retired incrementally.
// Leading dynamic-table capacity updates are applied in wire order. Updates
// after a field or above the configured maximum are decoding errors.
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
    // consuming a valid block and synchronizing its dynamic table.
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
            // A callback exception is the initiating failure. Continuing the
            // block is only a best-effort attempt to preserve HPACK table sync;
            // if that continuation also fails, the decoder rolls its transaction
            // back and the original callback exception still reaches its owner.
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

struct hpack_header_with_name_index_options final {
    bool never_indexed_{false};
};

class hpack_encoder final {
public:
    // Indexed representations require a nonzero index; zero throws
    // std::invalid_argument before changing output.
    static void encode_indexed(std::pmr::string& output, hpack_index index);
    static void encode_dynamic_table_size_update(std::pmr::string& output, hpack_table_size maximum);
    // Names and values may borrow output. Aliasing inputs are owned in output's
    // resource before appending can invalidate them.
    // Literal fields reserve their complete encoding before writing; allocation
    // failure leaves the existing output bytes unchanged.
    static void encode_header(
        std::pmr::string& output, std::string_view name, std::string_view value);
    // The same borrowing and reservation contracts apply; name_index must be nonzero.
    static void encode_header_with_name_index(std::pmr::string& output, hpack_index name_index,
        std::string_view value, hpack_header_with_name_index_options options = {});
    static void encode_status(std::pmr::string& output, http_status_code status);
};

}  // namespace ruvia
