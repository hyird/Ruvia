#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/Http1ClientResponseParser.h"
#include "ruvia/http/HttpResponseBodyDecoding.h"
#include "ruvia/http/detail/util/BorrowedView.h"

namespace ruvia {

enum class Http1ClientResponseBodyError : unsigned char {
    kInvalidFraming,
    kInvalidTransferCoding,
    kIncompleteBody,
    kNonEmpty205,
};

[[nodiscard]] std::string_view http1ClientResponseBodyErrorMessage(
    Http1ClientResponseBodyError error) noexcept;

class Http1ClientResponseBodyDecoder final {
public:
    class NeedInput final {
    public:
        [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
            return consumed_;
        }

    private:
        friend class Http1ClientResponseBodyDecoder;
        explicit constexpr NeedInput(std::size_t consumed) noexcept
            : consumed_(consumed) {}
        std::size_t consumed_;
    };

    class OutputView final {
    public:
        [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
            return bytes_;
        }
        std::string_view bytes() const&& = delete;

    private:
        friend class Http1ClientResponseBodyDecoder;
        constexpr OutputView(std::size_t consumed, std::string_view bytes) noexcept
            : consumed_(consumed),
              bytes_(bytes) {}
        std::size_t consumed_;
        std::string_view bytes_;
    };

    class ValidatedTrailersView final {
    public:
        [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr std::string_view bytes() const& noexcept {
            return bytes_;
        }
        std::string_view bytes() const&& = delete;

    private:
        friend class Http1ClientResponseBodyDecoder;
        constexpr ValidatedTrailersView(std::size_t consumed, std::string_view bytes) noexcept
            : consumed_(consumed),
              bytes_(bytes) {}
        std::size_t consumed_;
        std::string_view bytes_;
    };

    class Complete final {
    public:
        [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr Http1ClosePolicy persistence() const noexcept {
            return persistence_;
        }

    private:
        friend class Http1ClientResponseBodyDecoder;
        constexpr Complete(std::size_t consumed, Http1ClosePolicy persistence) noexcept
            : consumed_(consumed),
              persistence_(persistence) {}
        std::size_t consumed_;
        Http1ClosePolicy persistence_;
    };

    class ProtocolFailure final {
    public:
        [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
            return consumed_;
        }
        [[nodiscard]] constexpr Http1ClientResponseBodyError error() const noexcept {
            return error_;
        }

    private:
        friend class Http1ClientResponseBodyDecoder;
        constexpr ProtocolFailure(std::size_t consumed, Http1ClientResponseBodyError error) noexcept
            : consumed_(consumed),
              error_(error) {}
        std::size_t consumed_;
        Http1ClientResponseBodyError error_;
    };

    class DecoderFailure final {
    public:
        [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
            return consumed_;
        }

    private:
        friend class Http1ClientResponseBodyDecoder;
        explicit constexpr DecoderFailure(std::size_t consumed) noexcept
            : consumed_(consumed) {}
        std::size_t consumed_;
    };

    class Result final {
    public:
        [[nodiscard]] std::size_t consumedBytes() const noexcept;
        [[nodiscard]] const NeedInput* needInput() const& noexcept;
        const NeedInput* needInput() const&& = delete;
        [[nodiscard]] const OutputView* output() const& noexcept;
        const OutputView* output() const&& = delete;
        [[nodiscard]] const ValidatedTrailersView* trailers() const& noexcept;
        const ValidatedTrailersView* trailers() const&& = delete;
        [[nodiscard]] const Complete* complete() const& noexcept;
        const Complete* complete() const&& = delete;
        [[nodiscard]] const ProtocolFailure* protocolFailure() const& noexcept;
        const ProtocolFailure* protocolFailure() const&& = delete;
        [[nodiscard]] const DecoderFailure* decoderFailure() const& noexcept;
        const DecoderFailure* decoderFailure() const&& = delete;

    private:
        friend class Http1ClientResponseBodyDecoder;
        using Value = std::variant<NeedInput, OutputView, ValidatedTrailersView, Complete,
            ProtocolFailure, DecoderFailure>;
        template <typename T>
        explicit Result(T value) noexcept
            : value_(std::move(value)) {}
        Value value_;
    };

    Http1ClientResponseBodyDecoder(Http1ClientResponsePlan plan,
        std::pmr::memory_resource* resource);
    ~Http1ClientResponseBodyDecoder() = default;
    Http1ClientResponseBodyDecoder(const Http1ClientResponseBodyDecoder&) = delete;
    Http1ClientResponseBodyDecoder& operator=(const Http1ClientResponseBodyDecoder&) = delete;
    Http1ClientResponseBodyDecoder(Http1ClientResponseBodyDecoder&&) = delete;
    Http1ClientResponseBodyDecoder& operator=(Http1ClientResponseBodyDecoder&&) = delete;

    // Output borrows available without transfer coding and scratch otherwise;
    // trailers borrow available. Consume views before changing either storage
    // or making the next call. Erase exactly consumedBytes() from available.
    [[nodiscard]] Result decode(std::string_view available, std::span<char> scratch);
    // EOF is monotonic. Supply any remaining buffered bytes; later calls may
    // drain decoder output but can never request more network input.
    [[nodiscard]] Result finishInput(std::string_view available, std::span<char> scratch);
    template <detail::HttpTemporaryOwningCharString Input>
    Result decode(Input&&, std::span<char>) = delete;
    template <detail::HttpTemporaryOwningCharString Input>
    Result finishInput(Input&&, std::span<char>) = delete;

private:
    enum class Framing : unsigned char { kNoBody,
        kFixed,
        kChunked,
        kCloseDelimited };
    enum class TransferPhase : unsigned char { kNeedsFramedInput,
        kDrainOutput,
        kEnded };
    [[nodiscard]] Result step(std::string_view available, std::span<char> scratch, bool eof);
    [[nodiscard]] Result replayTerminal() const noexcept;
    [[nodiscard]] Result needInput(std::size_t consumed = 0) const noexcept;
    [[nodiscard]] Result emit(std::size_t consumed, std::string_view bytes) const noexcept;
    [[nodiscard]] Result validatedTrailers(std::size_t consumed, std::string_view bytes);
    [[nodiscard]] Result complete(std::size_t consumed);
    [[nodiscard]] Result fail(Http1ClientResponseBodyError error, std::size_t consumed = 0);
    [[nodiscard]] Result decoderFailure(std::size_t consumed = 0);
    [[nodiscard]] Result driveTransfer(
        std::string_view input, std::size_t wirePrefix, std::span<char> scratch);
    [[nodiscard]] Result finishTransfer(std::size_t consumed, std::span<char> scratch);

    using Terminal = std::variant<Complete, ProtocolFailure, DecoderFailure>;
    Framing framing_;
    Http1ClosePolicy persistence_{Http1ClosePolicy::kCloseAfterResponse};
    std::optional<HttpTransferCodingDecoder> transfer_;
    std::optional<HttpResponseChunkedBodyDecoder> chunked_;
    std::optional<Terminal> terminal_;
    std::size_t remaining_{0};
    // The chunk parser advances through a whole body view. These counters let
    // the caller retain its unconsumed wire suffix without storing a borrowed view.
    std::size_t pendingBodyBytes_{0};
    std::size_t pendingDelimiterBytes_{0};
    bool zeroContent_{false};
    bool eof_{false};
    bool trailersReported_{false};
    TransferPhase transferPhase_{TransferPhase::kNeedsFramedInput};
};

}  // namespace ruvia
