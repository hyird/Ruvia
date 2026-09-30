#pragma once

#include <zlib.h>

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

class HttpTransferCodingDecodeNeedInput final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class HttpTransferCodingDecodeResult;
    friend class HttpTransferCodingDecoder;
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
    friend class HttpTransferCodingDecoder;
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
    friend class HttpTransferCodingDecoder;
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
    friend class HttpTransferCodingDecoder;
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
    friend class HttpTransferCodingDecoder;
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
    friend class HttpTransferCodingDecoder;
    using Value = std::variant<HttpTransferCodingDecodeNeedInput, HttpTransferCodingDecodeOutputView, HttpTransferCodingDecodeComplete, HttpTransferCodingDecodeFailure, HttpTransferCodingDecoderFailure>;
    template <typename Result>
    explicit HttpTransferCodingDecodeResult(Result result) noexcept
        : value_(std::move(result)) {}
    Value value_;
};

// Role-neutral incremental decoder. Input is never retained; output views
// borrow the caller's scratch storage. Consume each view before reusing it.
// Keep the decoder at a stable address and its PMR resource alive until
// destruction. Drain output with decode({}, scratch) before requesting more
// input; finishInput() commits framing EOF after all input/output is drained.
class HttpTransferCodingDecoder final {
public:
    HttpTransferCodingDecoder(HttpTransferCoding coding, std::pmr::memory_resource* resource, ProtocolByteLimit decodedLimit);
    ~HttpTransferCodingDecoder();
    HttpTransferCodingDecoder(const HttpTransferCodingDecoder&) = delete;
    HttpTransferCodingDecoder& operator=(const HttpTransferCodingDecoder&) = delete;
    HttpTransferCodingDecoder(HttpTransferCodingDecoder&&) = delete;
    HttpTransferCodingDecoder& operator=(HttpTransferCodingDecoder&&) = delete;
    [[nodiscard]] HttpTransferCodingDecodeResult decode(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] HttpTransferCodingDecodeResult finishInput() noexcept;

private:
    struct InflateStep {
        std::size_t consumed{0};
        std::size_t produced{0};
        int status{Z_OK};
    };
    struct Active final {};
    struct GzipMemberBoundary final {};
    struct Complete final {};
    struct DecoderFailed final {};
    using State = std::variant<Active, GzipMemberBoundary, Complete, HttpTransferCodingDecodeError, DecoderFailed>;
    [[nodiscard]] InflateStep inflateStep(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] static HttpTransferCodingDecodeResult needInput(std::size_t consumed) noexcept;
    [[nodiscard]] static HttpTransferCodingDecodeResult output(std::size_t consumed, std::string_view bytes) noexcept;
    [[nodiscard]] static HttpTransferCodingDecodeResult complete(std::size_t consumed) noexcept;
    [[nodiscard]] HttpTransferCodingDecodeResult fail(std::size_t consumed, HttpTransferCodingDecodeError error) noexcept;
    [[nodiscard]] HttpTransferCodingDecodeResult failDecoder(std::size_t consumed) noexcept;
    static voidpf zallocThunk(voidpf opaque, uInt items, uInt size) noexcept;
    static void zfreeThunk(voidpf opaque, voidpf address) noexcept;
    z_stream stream_{};
    State state_{Active{}};
    std::pmr::memory_resource* resource_{nullptr};
    ProtocolByteLimit bodyLimit_;
    std::size_t decodedBytes_{0};
    HttpTransferCoding coding_;
};

}  // namespace ruvia
