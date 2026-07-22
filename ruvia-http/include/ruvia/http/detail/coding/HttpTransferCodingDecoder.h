#pragma once

#include "ruvia/http/detail/coding/ZlibPmrAllocation.h"

#include "ruvia/http/detail/coding/HttpTransferCoding.h"
#include "ruvia/http/HttpProtocolError.h"
#include "ruvia/http/detail/request/HttpRequestBodyFailure.h"
#include "ruvia/http/ProtocolByteLimit.h"

#include <cstddef>
#include <exception>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include <zlib.h>

namespace ruvia::detail {

inline constexpr std::size_t kBodyReadChunkBytes = 8 * 1024;

enum class TransferCodingDecodeError : std::uint8_t {
    kInvalidContent,
    kDecodedSizeExceeded,
    kDecoderFailure,
};

class TransferCodingDecodeNeedInput final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class TransferCodingDecodeResult;
    friend class TransferCodingDecoder;
    explicit constexpr TransferCodingDecodeNeedInput(
        std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}
    std::size_t consumedBytes_;
};

class TransferCodingDecodeOutput final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend class TransferCodingDecodeResult;
    friend class TransferCodingDecoder;
    constexpr TransferCodingDecodeOutput(
        std::size_t consumedBytes,
        std::string_view bytes) noexcept
        : consumedBytes_(consumedBytes), bytes_(bytes) {}
    std::size_t consumedBytes_;
    std::string_view bytes_;
};

class TransferCodingDecodeComplete final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class TransferCodingDecodeResult;
    friend class TransferCodingDecoder;
    explicit constexpr TransferCodingDecodeComplete(
        std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}
    std::size_t consumedBytes_;
};

class TransferCodingDecodeProtocolFailure final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

    [[nodiscard]] HttpProtocolError protocolError() const noexcept {
        switch (error_) {
            case TransferCodingDecodeError::kInvalidContent:
                return HttpProtocolError(http_status::kBadRequest, "invalid transfer-coding body");
            case TransferCodingDecodeError::kDecodedSizeExceeded:
                return HttpRequestBodyFailure::tooLarge().protocolError();
            case TransferCodingDecodeError::kDecoderFailure:
                std::terminate();
        }
        std::terminate();
    }

private:
    friend class TransferCodingDecodeResult;
    friend class TransferCodingDecoder;
    constexpr TransferCodingDecodeProtocolFailure(
        std::size_t consumedBytes,
        TransferCodingDecodeError error) noexcept
        : consumedBytes_(consumedBytes), error_(error) {}
    std::size_t consumedBytes_;
    TransferCodingDecodeError error_;
};

class TransferCodingDecoderFailure final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class TransferCodingDecodeResult;
    friend class TransferCodingDecoder;
    explicit constexpr TransferCodingDecoderFailure(
        std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}
    std::size_t consumedBytes_;
};

// One inflate step consumes a prefix of caller-owned input and exclusively
// requests more input, exposes output in the caller-owned span, completes, or
// reports a protocol failure or an internal decoder failure. No input view or
// output storage is kept by the decoder across calls.
class TransferCodingDecodeResult final {
public:
    [[nodiscard]] std::size_t consumedBytes() const noexcept {
        return std::visit(
            [](const auto& result) { return result.consumedBytes(); },
            value_);
    }

    [[nodiscard]] const TransferCodingDecodeNeedInput* needInput() const & noexcept {
        return std::get_if<TransferCodingDecodeNeedInput>(&value_);
    }
    const TransferCodingDecodeNeedInput* needInput() const && = delete;

    [[nodiscard]] const TransferCodingDecodeOutput* output() const & noexcept {
        return std::get_if<TransferCodingDecodeOutput>(&value_);
    }
    const TransferCodingDecodeOutput* output() const && = delete;

    [[nodiscard]] const TransferCodingDecodeComplete* complete() const & noexcept {
        return std::get_if<TransferCodingDecodeComplete>(&value_);
    }
    const TransferCodingDecodeComplete* complete() const && = delete;

    [[nodiscard]] const TransferCodingDecodeProtocolFailure*
    protocolFailure() const & noexcept {
        return std::get_if<TransferCodingDecodeProtocolFailure>(&value_);
    }
    const TransferCodingDecodeProtocolFailure*
    protocolFailure() const && = delete;

    [[nodiscard]] const TransferCodingDecoderFailure*
    decoderFailure() const & noexcept {
        return std::get_if<TransferCodingDecoderFailure>(&value_);
    }
    const TransferCodingDecoderFailure* decoderFailure() const && = delete;

private:
    friend class TransferCodingDecoder;
    using Value = std::variant<
        TransferCodingDecodeNeedInput,
        TransferCodingDecodeOutput,
        TransferCodingDecodeComplete,
        TransferCodingDecodeProtocolFailure,
        TransferCodingDecoderFailure>;

    template <typename Result>
    explicit TransferCodingDecodeResult(Result result) noexcept
        : value_(std::move(result)) {}

    Value value_;
};

class TransferCodingDecoder final {
public:
    TransferCodingDecoder(
        HttpTransferCoding coding,
        std::pmr::memory_resource* resource,
        ProtocolByteLimit bodyLimit);
    ~TransferCodingDecoder();

    TransferCodingDecoder(const TransferCodingDecoder&) = delete;
    TransferCodingDecoder& operator=(const TransferCodingDecoder&) = delete;

    [[nodiscard]] TransferCodingDecodeResult decode(
        std::string_view input,
        std::span<char> output) noexcept;
    // EOF is a decoder step, not a second status channel. It returns the same
    // exclusive complete/failure result as decode(), with zero consumed bytes.
    [[nodiscard]] TransferCodingDecodeResult finishInput() noexcept;

private:
    struct InflateStep {
        std::size_t consumed{0};
        std::size_t produced{0};
        int status{Z_OK};
    };

    struct Active final {};
    struct GzipMemberBoundary final {};
    struct Complete final {};
    using State = std::variant<
        Active,
        GzipMemberBoundary,
        Complete,
        TransferCodingDecodeError>;

    [[nodiscard]] InflateStep inflateStep(
        std::string_view input,
        std::span<char> output) noexcept;
    [[nodiscard]] static TransferCodingDecodeResult needInput(
        std::size_t consumed) noexcept;
    [[nodiscard]] static TransferCodingDecodeResult output(
        std::size_t consumed,
        std::string_view bytes) noexcept;
    [[nodiscard]] static TransferCodingDecodeResult complete(
        std::size_t consumed) noexcept;
    [[nodiscard]] TransferCodingDecodeResult fail(
        std::size_t consumed,
        TransferCodingDecodeError error) noexcept;


    static voidpf zallocThunk(voidpf opaque, uInt items, uInt size) noexcept;
    static void zfreeThunk(voidpf opaque, voidpf address) noexcept;

    z_stream stream_{};
    State state_{Active{}};
    std::pmr::memory_resource* resource_{nullptr};
    ProtocolByteLimit bodyLimit_;
    std::size_t decodedBytes_{0};
    HttpTransferCoding coding_;
};

}  // namespace ruvia::detail
