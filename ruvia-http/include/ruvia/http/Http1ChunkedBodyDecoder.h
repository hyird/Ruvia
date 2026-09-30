#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/Http1ChunkDecodeError.h"
#include "ruvia/http/ProtocolByteLimit.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

class Http1ChunkDecodeNeedMore final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class Http1ChunkDecodeResult;
    explicit constexpr Http1ChunkDecodeNeedMore(std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}
    std::size_t consumedBytes_;
};

class Http1ChunkDecodeBodyChunkView final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }
    [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

private:
    friend class Http1ChunkDecodeResult;
    constexpr Http1ChunkDecodeBodyChunkView(std::size_t consumedBytes, std::string_view bytes) noexcept
        : consumedBytes_(consumedBytes),
          bytes_(bytes) {}
    std::size_t consumedBytes_;
    std::string_view bytes_;
};

class Http1ChunkDecodeCompleteView final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }
    [[nodiscard]] constexpr std::string_view trailers() const& noexcept {
        return trailers_;
    }
    std::string_view trailers() const&& = delete;

private:
    friend class Http1ChunkDecodeResult;
    explicit constexpr Http1ChunkDecodeCompleteView(
        std::size_t consumedBytes, std::string_view trailers) noexcept
        : consumedBytes_(consumedBytes),
          trailers_(trailers) {}
    std::size_t consumedBytes_;
    std::string_view trailers_;
};

class Http1ChunkDecodeFailure final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }
    [[nodiscard]] constexpr Http1ChunkDecodeError error() const noexcept {
        return error_;
    }

private:
    friend class Http1ChunkDecodeResult;
    constexpr Http1ChunkDecodeFailure(
        std::size_t consumedBytes, Http1ChunkDecodeError error) noexcept
        : consumedBytes_(consumedBytes),
          error_(error) {}
    std::size_t consumedBytes_;
    Http1ChunkDecodeError error_;
};

class Http1ChunkDecodeResult final {
public:
    [[nodiscard]] std::size_t consumedBytes() const noexcept {
        return std::visit([](const auto& result) { return result.consumedBytes(); }, value_);
    }
    [[nodiscard]] const Http1ChunkDecodeNeedMore* needMore() const& noexcept {
        return std::get_if<Http1ChunkDecodeNeedMore>(&value_);
    }
    const Http1ChunkDecodeNeedMore* needMore() const&& = delete;
    [[nodiscard]] const Http1ChunkDecodeBodyChunkView* bodyChunk() const& noexcept {
        return std::get_if<Http1ChunkDecodeBodyChunkView>(&value_);
    }
    const Http1ChunkDecodeBodyChunkView* bodyChunk() const&& = delete;
    [[nodiscard]] const Http1ChunkDecodeCompleteView* complete() const& noexcept {
        return std::get_if<Http1ChunkDecodeCompleteView>(&value_);
    }
    const Http1ChunkDecodeCompleteView* complete() const&& = delete;
    [[nodiscard]] const Http1ChunkDecodeFailure* failure() const& noexcept {
        return std::get_if<Http1ChunkDecodeFailure>(&value_);
    }
    const Http1ChunkDecodeFailure* failure() const&& = delete;

private:
    friend class Http1ChunkedBodyDecoder;
    using Value = std::variant<Http1ChunkDecodeNeedMore, Http1ChunkDecodeBodyChunkView,
        Http1ChunkDecodeCompleteView, Http1ChunkDecodeFailure>;
    template <typename Result>
    explicit Http1ChunkDecodeResult(Result result) noexcept
        : value_(std::move(result)) {}
    [[nodiscard]] static Http1ChunkDecodeResult makeNeedMore(std::size_t consumedBytes) noexcept;
    [[nodiscard]] static Http1ChunkDecodeResult makeBodyChunk(
        std::size_t consumedBytes, std::string_view bytes) noexcept;
    [[nodiscard]] static Http1ChunkDecodeResult makeComplete(
        std::size_t consumedBytes, std::string_view trailers = {}) noexcept;
    [[nodiscard]] static Http1ChunkDecodeResult makeFailure(
        std::size_t consumedBytes, Http1ChunkDecodeError error) noexcept;
    Value value_;
};

enum class Http1ChunkTrailerRole : std::uint8_t {
    kRequest,
    kResponse,
};

struct Http1ChunkedBodyDecoderConfig final {
    ProtocolByteLimit bodyLimit{ProtocolByteLimit::unlimited()};
    Http1ChunkTrailerRole trailerRole{Http1ChunkTrailerRole::kRequest};
};

// Incremental sans-I/O HTTP/1 chunk framing decoder. Payload and trailer views
// borrow the supplied input and remain valid only until it is modified. The
// body limit counts chunk payload bytes, which may still be transfer-encoded.
class Http1ChunkedBodyDecoder final {
public:
    explicit Http1ChunkedBodyDecoder(Http1ChunkedBodyDecoderConfig config = {});
    ~Http1ChunkedBodyDecoder();
    Http1ChunkedBodyDecoder(const Http1ChunkedBodyDecoder&) = delete;
    Http1ChunkedBodyDecoder& operator=(const Http1ChunkedBodyDecoder&) = delete;
    Http1ChunkedBodyDecoder(Http1ChunkedBodyDecoder&&) noexcept;
    Http1ChunkedBodyDecoder& operator=(Http1ChunkedBodyDecoder&&) noexcept;

    [[nodiscard]] Http1ChunkDecodeResult decode(std::string_view available);
    [[nodiscard]] Http1ChunkDecodeResult decode(std::string_view available, std::size_t maxBodyBytes);
    template <detail::HttpTemporaryOwningCharString Input>
    Http1ChunkDecodeResult decode(Input&&) = delete;
    template <detail::HttpTemporaryOwningCharString Input>
    Http1ChunkDecodeResult decode(Input&&, std::size_t) = delete;

private:
    enum class ProgressState : std::uint8_t {
        kSizeLine,
        kBody,
        kDelimiter,
        kTrailers,
        kComplete,
    };
    using State = std::expected<ProgressState, Http1ChunkDecodeError>;

    [[nodiscard]] Http1ChunkDecodeResult fail(
        std::size_t consumedBytes, Http1ChunkDecodeError error) noexcept;
    [[nodiscard]] std::optional<Http1ChunkDecodeError> accountFraming(std::size_t bytes) noexcept;
    [[nodiscard]] std::optional<Http1ChunkDecodeError> consumeDelimiter(
        std::string_view available) noexcept;
    [[nodiscard]] bool trailersValid(std::string_view trailers) const;

    ProtocolByteLimit bodyLimit_;
    Http1ChunkTrailerRole trailerRole_;
    State state_{ProgressState::kSizeLine};
    std::size_t trailerSearchOffset_{0};
    std::size_t remaining_{0};
    std::size_t decodedBytes_{0};
    std::size_t encodedOverheadBytes_{0};
};

}  // namespace ruvia
