#pragma once

#include "ruvia/http/detail/http1/Http1ServerConnectionPlan.h"
#include "ruvia/web/detail/BorrowedView.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>
#include <utility>
#include <variant>

namespace ruvia::detail {

class Http1SessionRequestCompletion;

// The connection is closing, so no bytes from this request can be reused.
class Http1RequestBufferDiscarded final {
private:
    friend class Http1SessionRequestCompletion;

    constexpr Http1RequestBufferDiscarded() noexcept = default;
};

// The session still owns an unshifted read buffer and must remove exactly this
// request prefix before parsing the next pipelined request.
class Http1RequestBufferCompaction final {
public:
    [[nodiscard]] constexpr std::size_t consumedBytes() const noexcept {
        return consumedBytes_;
    }

private:
    friend class Http1SessionRequestCompletion;

    explicit constexpr Http1RequestBufferCompaction(
        std::size_t consumedBytes) noexcept
        : consumedBytes_(consumedBytes) {}

    std::size_t consumedBytes_;
};

// A request-body runtime handed its pipelined suffix over instead of writing it
// into the connection read buffer, whose bytes still back every view in the
// request being completed. The session installs these bytes at its single
// buffer-cleanup point, once the response is written and the access log is
// recorded. The view borrows request-scoped storage that outlives the
// completion.
class Http1RequestBufferPipelineRestore final {
public:
    [[nodiscard]] constexpr std::string_view pipeline() const noexcept {
        return pipeline_;
    }

private:
    friend class Http1SessionRequestCompletion;

    explicit constexpr Http1RequestBufferPipelineRestore(
        std::string_view pipeline) noexcept
        : pipeline_(pipeline) {}

    std::string_view pipeline_;
};

class Http1RequestBufferCompletion final {
public:
    [[nodiscard]] constexpr const Http1RequestBufferDiscarded*
    discarded() const & noexcept {
        return std::get_if<Http1RequestBufferDiscarded>(&value_);
    }
    [[nodiscard]] constexpr const Http1RequestBufferDiscarded*
    discarded() const && = delete;

    [[nodiscard]] constexpr const Http1RequestBufferCompaction*
    compaction() const & noexcept {
        return std::get_if<Http1RequestBufferCompaction>(&value_);
    }
    [[nodiscard]] constexpr const Http1RequestBufferCompaction*
    compaction() const && = delete;

    [[nodiscard]] constexpr const Http1RequestBufferPipelineRestore*
    pipelineRestore() const & noexcept {
        return std::get_if<Http1RequestBufferPipelineRestore>(&value_);
    }
    [[nodiscard]] constexpr const Http1RequestBufferPipelineRestore*
    pipelineRestore() const && = delete;

private:
    friend class Http1SessionRequestCompletion;

    using Value = std::variant<
        Http1RequestBufferDiscarded,
        Http1RequestBufferCompaction,
        Http1RequestBufferPipelineRestore>;

    template <typename Alternative>
    explicit constexpr Http1RequestBufferCompletion(
        Alternative alternative) noexcept
        : value_(std::move(alternative)) {}

    Value value_;
};

// A buffered response still needs the session's scatter-gather writer.
class Http1BufferedResponseReady final {
private:
    friend class Http1SessionRequestCompletion;

    constexpr Http1BufferedResponseReady() noexcept = default;
};

// A response-stream route already committed its final head. Only this
// alternative exposes the exact wire status used by access logging.
class Http1CommittedStreamResponse final {
public:
    [[nodiscard]] constexpr std::uint16_t status() const noexcept {
        return status_;
    }

private:
    friend class Http1SessionRequestCompletion;

    explicit constexpr Http1CommittedStreamResponse(
        std::uint16_t status) noexcept
        : status_(status) {}

    std::uint16_t status_;
};

// One terminal result for a dispatched HTTP/1 request. Wire ownership,
// connection reuse, and read-buffer cleanup are committed together so the
// session never reconstructs them from an optional status plus scalar flags.
class Http1SessionRequestCompletion final {
public:
    [[nodiscard]] static Http1SessionRequestCompletion makeBufferedClosing(
        Http1ServerConnectionPlan connectionPlan) noexcept {
        if (connectionPlan.disposition() !=
            Http1ConnectionDisposition::kClose) {
            std::terminate();
        }
        return Http1SessionRequestCompletion(
            Http1BufferedResponseReady{},
            connectionPlan,
            Http1RequestBufferCompletion(
                Http1RequestBufferDiscarded{}));
    }

    [[nodiscard]] static Http1SessionRequestCompletion
    makeBufferedUnrestored(
        Http1ServerConnectionPlan connectionPlan,
        std::size_t consumedBytes) noexcept {
        return Http1SessionRequestCompletion(
            Http1BufferedResponseReady{},
            connectionPlan,
            unshiftedBufferCompletion(connectionPlan, consumedBytes));
    }

    [[nodiscard]] static Http1SessionRequestCompletion
    makeBufferedPipelineRestore(
        Http1ServerConnectionPlan connectionPlan,
        std::string_view pipeline) noexcept {
        if (connectionPlan.disposition() !=
            Http1ConnectionDisposition::kReuse) {
            std::terminate();
        }
        return Http1SessionRequestCompletion(
            Http1BufferedResponseReady{},
            connectionPlan,
            Http1RequestBufferCompletion(
                Http1RequestBufferPipelineRestore(pipeline)));
    }

    template <RvalueCharBasicString Pipeline>
    static Http1SessionRequestCompletion makeBufferedPipelineRestore(
        Http1ServerConnectionPlan,
        Pipeline&&) = delete;

    [[nodiscard]] static Http1SessionRequestCompletion makeCommittedStream(
        Http1ServerConnectionPlan connectionPlan,
        std::uint16_t status,
        std::size_t consumedBytes) noexcept {
        return Http1SessionRequestCompletion(
            Http1CommittedStreamResponse(status),
            connectionPlan,
            unshiftedBufferCompletion(connectionPlan, consumedBytes));
    }

    [[nodiscard]] constexpr const Http1BufferedResponseReady*
    bufferedResponse() const & noexcept {
        return std::get_if<Http1BufferedResponseReady>(&value_);
    }
    [[nodiscard]] constexpr const Http1BufferedResponseReady*
    bufferedResponse() const && = delete;

    [[nodiscard]] constexpr const Http1CommittedStreamResponse*
    committedStream() const & noexcept {
        return std::get_if<Http1CommittedStreamResponse>(&value_);
    }
    [[nodiscard]] constexpr const Http1CommittedStreamResponse*
    committedStream() const && = delete;

    [[nodiscard]] constexpr Http1ServerConnectionPlan
    connectionPlan() const noexcept {
        return connectionPlan_;
    }

    [[nodiscard]] constexpr const Http1RequestBufferCompletion&
    bufferCompletion() const & noexcept {
        return bufferCompletion_;
    }
    [[nodiscard]] constexpr const Http1RequestBufferCompletion&
    bufferCompletion() const && = delete;

private:
    using Value = std::variant<
        Http1BufferedResponseReady,
        Http1CommittedStreamResponse>;

    [[nodiscard]] static Http1RequestBufferCompletion
    unshiftedBufferCompletion(
        Http1ServerConnectionPlan connectionPlan,
        std::size_t consumedBytes) noexcept {
        if (connectionPlan.disposition() ==
            Http1ConnectionDisposition::kClose) {
            return Http1RequestBufferCompletion(
                Http1RequestBufferDiscarded{});
        }
        return Http1RequestBufferCompletion(
            Http1RequestBufferCompaction(consumedBytes));
    }

    template <typename Alternative>
    Http1SessionRequestCompletion(
        Alternative alternative,
        Http1ServerConnectionPlan connectionPlan,
        Http1RequestBufferCompletion bufferCompletion) noexcept
        : value_(std::move(alternative)),
          connectionPlan_(connectionPlan),
          bufferCompletion_(std::move(bufferCompletion)) {}

    Value value_;
    Http1ServerConnectionPlan connectionPlan_;
    Http1RequestBufferCompletion bufferCompletion_;
};

}  // namespace ruvia::detail
