#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/hpack_protocol_types.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

struct hpack_decoder_options final {
    // Must outlive the decoder, including its implementation and dynamic table.
    // nullptr binds the current default PMR resource at construction.
    std::pmr::memory_resource* resource_{nullptr};
};

// Stateful RFC 7541 decoder. One decode() call is one dynamic-table
// transaction; protocol failures roll back table changes while callback
// rejection still consumes the complete block to keep peer state synchronized.
class hpack_decoder final {
public:
    explicit hpack_decoder(hpack_decoder_options options = {});
    ~hpack_decoder();
    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;
    hpack_decoder(hpack_decoder&&) noexcept;
    hpack_decoder& operator=(hpack_decoder&&) noexcept;

    void set_max_dynamic_table_size(std::size_t bytes);

    // Header name/value views are borrowed only for the callback invocation.
    // Copy into owned storage when retaining a field beyond that invocation.
    template <typename callback>
        requires std::predicate<callback&, std::string_view, std::string_view>
    [[nodiscard]] hpack_decode_result decode(std::string_view block, callback&& callback_value) {
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
    [[nodiscard]] hpack_decode_result decode_with_callback(
        std::string_view block, void* target, header_callback_type callback);

    class impl_type;
    struct impl_deleter_type {
        void operator()(impl_type* value) const noexcept;
    };
    std::unique_ptr<impl_type, impl_deleter_type> impl_;
};

struct hpack_header_with_name_index_options final {
    bool never_indexed_{false};
};

class hpack_encoder final {
public:
    static void encode_indexed(std::pmr::string& output, std::uint32_t index);
    static void encode_dynamic_table_size_update(std::pmr::string& output, std::uint32_t maximum);
    static void encode_header(
        std::pmr::string& output, std::string_view name, std::string_view value);
    static void encode_header_with_name_index(std::pmr::string& output, std::uint32_t name_index,
        std::string_view value, hpack_header_with_name_index_options options = {});
    static void encode_status(std::pmr::string& output, http_status_code status);
};

}  // namespace ruvia
