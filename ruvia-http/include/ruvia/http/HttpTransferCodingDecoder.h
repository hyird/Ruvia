#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/HttpTransferCoding.h"
#include "ruvia/http/HttpTransferCodingDecodeError.h"
#include "ruvia/http/ProtocolByteLimit.h"

namespace ruvia {

namespace detail {
class transfer_coding_decoder;
}

class HttpTransferCodingDecodeNeedInput final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class HttpTransferCodingDecodeResult;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr HttpTransferCodingDecodeNeedInput(std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}
    std::size_t consumedBytes_;
};

class HttpTransferCodingDecodeOutputView final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    friend class HttpTransferCodingDecodeResult;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr HttpTransferCodingDecodeOutputView(std::size_t consumedBytes, std::string_view bytes) noexcept
        : consumedBytes_(consumedBytes),
          bytes_(bytes) {}
    std::size_t consumedBytes_;
    std::string_view bytes_;
};

class HttpTransferCodingDecodeComplete final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class HttpTransferCodingDecodeResult;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr HttpTransferCodingDecodeComplete(std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}
    std::size_t consumedBytes_;
};

class HttpTransferCodingDecodeFailure final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }
    [[nodiscard]] constexpr HttpTransferCodingDecodeError error() const noexcept {
        return error_;
    }

private:
    friend class HttpTransferCodingDecodeResult;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    constexpr HttpTransferCodingDecodeFailure(std::size_t consumedBytes, HttpTransferCodingDecodeError error) noexcept
        : consumedBytes_(consumedBytes),
          error_(error) {}
    std::size_t consumedBytes_;
    HttpTransferCodingDecodeError error_;
};

class HttpTransferCodingDecoderFailure final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class HttpTransferCodingDecodeResult;
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    explicit constexpr HttpTransferCodingDecoderFailure(std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}
    std::size_t consumedBytes_;
};

class HttpTransferCodingDecodeResult final {
public:
    [[nodiscard]] std::size_t consumedBytes() const noexcept {
        return std::visit([](const auto& result) { return result.consumedBytes(); }, value_);
    }
    [[nodiscard]] const HttpTransferCodingDecodeNeedInput* needInput() const& noexcept {
        return std::get_if<HttpTransferCodingDecodeNeedInput>(&value_);
    }
    const HttpTransferCodingDecodeNeedInput* needInput() const&& = delete;
    [[nodiscard]] const HttpTransferCodingDecodeOutputView* output() const& noexcept {
        return std::get_if<HttpTransferCodingDecodeOutputView>(&value_);
    }
    const HttpTransferCodingDecodeOutputView* output() const&& = delete;
    [[nodiscard]] const HttpTransferCodingDecodeComplete* complete() const& noexcept {
        return std::get_if<HttpTransferCodingDecodeComplete>(&value_);
    }
    const HttpTransferCodingDecodeComplete* complete() const&& = delete;
    [[nodiscard]] const HttpTransferCodingDecodeFailure* failure() const& noexcept {
        return std::get_if<HttpTransferCodingDecodeFailure>(&value_);
    }
    const HttpTransferCodingDecodeFailure* failure() const&& = delete;
    [[nodiscard]] const HttpTransferCodingDecoderFailure* decoderFailure() const& noexcept {
        return std::get_if<HttpTransferCodingDecoderFailure>(&value_);
    }
    const HttpTransferCodingDecoderFailure* decoderFailure() const&& = delete;

private:
    friend class detail::transfer_coding_decoder;
    friend class http_transfer_coding_stack_decoder;
    using Value = std::variant<HttpTransferCodingDecodeNeedInput, HttpTransferCodingDecodeOutputView, HttpTransferCodingDecodeComplete, HttpTransferCodingDecodeFailure, HttpTransferCodingDecoderFailure>;
    template <typename Result>
    explicit HttpTransferCodingDecodeResult(Result result) noexcept
        : value_(std::move(result)) {}
    Value value_;
};

// The sole transfer-decoding entry point for one or more codings. Copies the
// protocol-order sequence during construction and decodes it in reverse.
// Input is never retained; output views borrow the caller's scratch storage.
// Consume each view before reusing it. Keep the decoder at a stable address and
// its PMR resource alive until destruction. Drain output with decode({}, scratch)
// before requesting more input; finish_input() commits framing EOF only after
// all input/output is drained. Each layer enforces decoded_limit independently.
// decode/finish_input return typed failures because incremental drivers must
// retain the exact wire consumption even when a layer fails; failure is terminal.
class http_transfer_coding_stack_decoder final {
public:
    http_transfer_coding_stack_decoder(std::span<const HttpTransferCoding> codings,
        std::pmr::memory_resource* resource, ProtocolByteLimit decoded_limit);
    ~http_transfer_coding_stack_decoder();
    http_transfer_coding_stack_decoder(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder& operator=(const http_transfer_coding_stack_decoder&) = delete;
    http_transfer_coding_stack_decoder(http_transfer_coding_stack_decoder&&) = delete;
    http_transfer_coding_stack_decoder& operator=(http_transfer_coding_stack_decoder&&) = delete;
    [[nodiscard]] HttpTransferCodingDecodeResult decode(
        std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] HttpTransferCodingDecodeResult finish_input() noexcept;

private:
    struct impl;
    impl* impl_{nullptr};
};

}  // namespace ruvia
