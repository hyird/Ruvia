#pragma once

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/HttpStatus.h"

namespace ruvia {

enum class HpackDecodeError : std::uint8_t {
    kNeedMore,
    kIntegerOverflow,
    kInvalidIndex,
    kInvalidString,
    kInvalidHuffman,
    kDynamicTableSize,
    kCallbackRejected,
};

class HpackDecodeResult final {
public:
    [[nodiscard]] constexpr bool decoded() const noexcept {
        return !error_.has_value();
    }
    [[nodiscard]] constexpr std::optional<HpackDecodeError> error() const noexcept {
        return error_;
    }

private:
    friend class HpackDecoder;
    explicit constexpr HpackDecodeResult(std::optional<HpackDecodeError> error) noexcept
        : error_(error) {}
    std::optional<HpackDecodeError> error_;
};

struct HpackDecoderOptions final {
    std::pmr::memory_resource* resource{nullptr};
};

// Stateful RFC 7541 decoder. One decode() call is one dynamic-table
// transaction; protocol failures roll back table changes while callback
// rejection still consumes the complete block to keep peer state synchronized.
class HpackDecoder final {
public:
    explicit HpackDecoder(HpackDecoderOptions options = {});
    ~HpackDecoder();
    HpackDecoder(const HpackDecoder&) = delete;
    HpackDecoder& operator=(const HpackDecoder&) = delete;
    HpackDecoder(HpackDecoder&&) noexcept;
    HpackDecoder& operator=(HpackDecoder&&) noexcept;

    void setMaxDynamicTableSize(std::size_t bytes);

    template <typename Callback>
        requires std::predicate<Callback&, std::string_view, std::string_view>
    [[nodiscard]] HpackDecodeResult decode(std::string_view block, Callback&& callback) {
        // Always pass an object through the erased callback boundary. Taking
        // addressof(callback) directly would produce a function pointer when a
        // free function is supplied, which cannot be represented by void*.
        std::exception_ptr callbackFailure;
        auto invocation = [&callback, &callbackFailure](
                              std::string_view name, std::string_view value) {
            if (callbackFailure) {
                return true;
            }
            try {
                return static_cast<bool>(std::invoke(callback, name, value));
            } catch (...) {
                callbackFailure = std::current_exception();
                // Keep consuming the block so dynamic-table state remains
                // synchronized before the user's exception is rethrown.
                return true;
            }
        };
        using Invocation = decltype(invocation);
        try {
            auto result = decodeWithCallback(block, std::addressof(invocation),
                [](void* target, std::string_view name, std::string_view value) {
                    return std::invoke(*static_cast<Invocation*>(target), name, value);
                });
            if (callbackFailure) {
                std::rethrow_exception(callbackFailure);
            }
            return result;
        } catch (...) {
            // A callback exception is the initiating failure. Continuing the
            // block is only a best-effort attempt to preserve HPACK table sync;
            // if that continuation also fails, the decoder rolls its transaction
            // back and the original callback exception still reaches its owner.
            if (callbackFailure) {
                std::rethrow_exception(callbackFailure);
            }
            throw;
        }
    }

private:
    using HeaderCallback = bool (*)(void*, std::string_view, std::string_view);
    [[nodiscard]] HpackDecodeResult decodeWithCallback(
        std::string_view block, void* target, HeaderCallback callback);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct HpackHeaderWithNameIndexOptions final {
    bool neverIndexed{false};
};

class HpackEncoder final {
public:
    static void encodeIndexed(std::pmr::string& output, std::uint32_t index);
    static void encodeDynamicTableSizeUpdate(std::pmr::string& output, std::uint32_t maximum);
    static void encodeHeader(
        std::pmr::string& output, std::string_view name, std::string_view value);
    static void encodeHeaderWithNameIndex(std::pmr::string& output, std::uint32_t nameIndex,
        std::string_view value, HpackHeaderWithNameIndexOptions options = {});
    static void encodeStatus(std::pmr::string& output, HttpStatusCode status);
};

}  // namespace ruvia
