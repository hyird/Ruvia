#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "ruvia/http/Http1ClientRequestWriter.h"
#include "ruvia/http/HttpHeader.h"

namespace ruvia {
enum class Http1RequestContentWriteError : std::uint8_t {
    kAwaitingContinue,
    kStopped,
    kWritePending,
    kNoWritePending,
    kLengthOverflow,
    kLengthMismatch,
    kInvalidTrailer,
    kTrailersRequireChunked,
    kOutputTooSmall,
    kTrailerLimit,
    kCommitMismatch,
};

// Bound to a successfully prepared streaming request's immutable framing facts.
// Payload bytes remain borrowed until commitChunk(); prefixes/suffixes are
// independent scatter-gather segments. A transport failure must abort the writer.
// releaseContent() is called on 100 Continue or on the driver's finite timeout.
// A final response before upload completion requires abort(), not an implicit FIN.
class Http1RequestContentWriter final {
public:
    struct Chunk final {
        std::array<char, 2 * sizeof(std::size_t) + 2> prefix{};
        std::size_t prefixSize{0};
        std::span<const char> payload{};
        std::string_view suffix{};
    };
    explicit Http1RequestContentWriter(const Http1ClientStreamingRequestContent& plan) noexcept;
    void releaseContent() noexcept;
    void abort() noexcept;
    [[nodiscard]] std::expected<Chunk, Http1RequestContentWriteError> planChunk(std::span<const char> payload) noexcept;
    [[nodiscard]] std::expected<void, Http1RequestContentWriteError> commitChunk(std::size_t payloadBytes) noexcept;
    // Returns a view into buffer. Transmit it, then commitFinish(). Known-length
    // bodies return an empty view and still require a successful commitFinish().
    [[nodiscard]] std::expected<std::string_view, Http1RequestContentWriteError> planFinish(
        std::span<char> buffer, std::span<const HttpHeaderView> trailers = {}) noexcept;
    [[nodiscard]] std::expected<void, Http1RequestContentWriteError> commitFinish() noexcept;
    [[nodiscard]] bool finished() const noexcept {
        return finished_;
    }
    [[nodiscard]] std::uint64_t committedPayloadBytes() const noexcept {
        return committed_;
    }

private:
    std::optional<std::uint64_t> length_;
    std::uint64_t committed_{0};
    std::size_t pending_{0};
    bool gated_{false}, stopped_{false}, writing_{false}, finishing_{false}, finished_{false};
};
}  // namespace ruvia
