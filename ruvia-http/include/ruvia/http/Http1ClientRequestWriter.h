#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "ruvia/http/Http1ClientExchangeState.h"
#include "ruvia/http/Http1ClosePolicy.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"

namespace ruvia {

class Http1ClientResponseParser;

// One immutable wire policy for request preparation. Expect is writer-owned so
// the emitted field and the resulting content gate cannot disagree. How long an
// I/O runtime waits before releasing a continue-gated body remains runtime
// policy, not an HTTP message-model setting.
struct Http1ClientRequestWirePolicy final {
    Http1ClosePolicy closePolicy{Http1ClosePolicy::kAllowReuse};
    HttpClientRequestExpectation expectation{HttpClientRequestExpectation::kNone};
};

namespace detail {

struct Http1ClientRequestPrepareResultAccess;

}  // namespace detail

struct Http1ClientRequestHeadView final {
    BorrowedText method{"GET"};
    BorrowedText target{"/"};
    std::span<const HttpHeaderView> headers{};
    // A value sends Content-Length; absent sends Transfer-Encoding: chunked.
    std::optional<std::uint64_t> contentLength{};
};

class Http1ClientStreamingRequestContent final {
public:
    [[nodiscard]] std::optional<std::uint64_t> contentLength() const noexcept {
        return length_;
    }
    [[nodiscard]] bool continueGated() const noexcept {
        return gated_;
    }

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;
    Http1ClientStreamingRequestContent(std::optional<std::uint64_t> length, bool gated) noexcept
        : length_(length),
          gated_(gated) {}
    std::optional<std::uint64_t> length_;
    bool gated_;
};

class Http1ClientRequestWithoutContent final {
private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    constexpr Http1ClientRequestWithoutContent() noexcept = default;
};

class Http1ClientImmediateRequestContent final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    explicit constexpr Http1ClientImmediateRequestContent(std::string_view bytes) noexcept
        : bytes_(bytes) {}

    std::string_view bytes_;
};

class Http1ClientContinueGatedRequestContent final {
public:
    [[nodiscard]] constexpr std::string_view bytes() const noexcept {
        return bytes_;
    }

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    explicit constexpr Http1ClientContinueGatedRequestContent(std::string_view bytes) noexcept
        : bytes_(bytes) {}

    std::string_view bytes_;
};

// Immutable outbound content contract. Continue-gated content means the writer
// generated Expect: 100-continue and the I/O owner must send the head separately;
// it may release those bytes after 100 Continue or according to its finite wait
// policy. Immediate content includes explicit empty content (Content-Length: 0).
// Payload exists only on the two alternatives that actually send content.
class Http1ClientRequestContentPlan final {
public:
    [[nodiscard]] constexpr const Http1ClientRequestWithoutContent* withoutContent()
        const& noexcept {
        return std::get_if<Http1ClientRequestWithoutContent>(&content_);
    }
    const Http1ClientRequestWithoutContent* withoutContent() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientImmediateRequestContent* immediate() const& noexcept {
        return std::get_if<Http1ClientImmediateRequestContent>(&content_);
    }
    const Http1ClientImmediateRequestContent* immediate() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientContinueGatedRequestContent* continueGated()
        const& noexcept {
        return std::get_if<Http1ClientContinueGatedRequestContent>(&content_);
    }
    const Http1ClientContinueGatedRequestContent* continueGated() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientStreamingRequestContent* streaming() const& noexcept {
        return std::get_if<Http1ClientStreamingRequestContent>(&content_);
    }
    const Http1ClientStreamingRequestContent* streaming() const&& = delete;

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    explicit Http1ClientRequestContentPlan(Http1ClientStreamingRequestContent content) noexcept
        : content_(content) {}
    using Content = std::variant<Http1ClientRequestWithoutContent,
        Http1ClientImmediateRequestContent, Http1ClientContinueGatedRequestContent, Http1ClientStreamingRequestContent>;

    explicit constexpr Http1ClientRequestContentPlan(
        Http1ClientRequestWithoutContent content) noexcept
        : content_(content) {}

    explicit constexpr Http1ClientRequestContentPlan(
        Http1ClientImmediateRequestContent content) noexcept
        : content_(content) {}

    explicit constexpr Http1ClientRequestContentPlan(
        Http1ClientContinueGatedRequestContent content) noexcept
        : content_(content) {}

    Content content_;
};

enum class Http1ClientRequestPrepareError : std::uint8_t {
    kInvalidMethod,
    kInvalidTarget,
    kConnectRequiresDedicatedEntry,
    kInvalidConnectOrigin,
    kInvalidHeader,
    kTooManyHeaders,
    kHostHeaderManagedByWriter,
    kContentLengthManagedByWriter,
    kTransferEncodingUnsupported,
    kTrailerSectionUnsupported,
    kExpectHeaderManagedByWriter,
    kInvalidConnection,
    kInvalidUpgrade,
    kUpgradeConnectionOptionRequired,
    kTeConnectionOptionRequired,
    kExpectationWithoutContent,
    kContentForbiddenForMethod,
    kOptionsContentTypeRequired,
    kHeaderTooLarge,
    kInvalidClosePolicy,
    kInvalidExpectation,
};

[[nodiscard]] std::string_view http1ClientRequestPrepareErrorMessage(
    Http1ClientRequestPrepareError error) noexcept;

class Http1ClientRequestBufferTooSmall final {
public:
    [[nodiscard]] constexpr std::size_t requiredHeadBytes() const noexcept {
        return requiredHeadBytes_;
    }

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    explicit constexpr Http1ClientRequestBufferTooSmall(std::size_t requiredHeadBytes) noexcept
        : requiredHeadBytes_(requiredHeadBytes) {}

    std::size_t requiredHeadBytes_;
};

// Transactionally prepared scatter-gather request. `head()` points into the
// caller-provided output buffer and the active immediate/continue-gated
// alternative points into the request's borrowed content; those sources must
// remain alive and unchanged until sent.
// exchangeState() returns independent response-side protocol facts; head() and
// contentPlan() remain readable for the request write or an explicit retry.
class PreparedHttp1ClientRequest final {
public:
    [[nodiscard]] constexpr std::string_view head() const& noexcept {
        return head_;
    }
    [[nodiscard]] constexpr std::string_view head() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientRequestContentPlan& contentPlan() const& noexcept {
        return contentPlan_;
    }
    [[nodiscard]] constexpr const Http1ClientRequestContentPlan& contentPlan() const&& = delete;

    [[nodiscard]] Http1ClientExchangeState exchangeState() const& {
        return Http1ClientExchangeState(
            exchangeState_, exchangeState_.offeredUpgradeProtocols_.get_allocator().resource());
    }
    Http1ClientExchangeState exchangeState() const&& = delete;

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    PreparedHttp1ClientRequest(std::string_view head, Http1ClientRequestContentPlan contentPlan,
        Http1ClientExchangeState exchangeState) noexcept
        : head_(head),
          contentPlan_(contentPlan),
          exchangeState_(std::move(exchangeState)) {}

    std::string_view head_;
    Http1ClientRequestContentPlan contentPlan_;
    Http1ClientExchangeState exchangeState_;
};

class Http1ClientRequestPrepareFailure final {
public:
    [[nodiscard]] constexpr Http1ClientRequestPrepareError error() const noexcept {
        return error_;
    }

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    explicit constexpr Http1ClientRequestPrepareFailure(
        Http1ClientRequestPrepareError error) noexcept
        : error_(error) {}

    Http1ClientRequestPrepareError error_;
};

class Http1ClientRequestPrepareResult final {
public:
    [[nodiscard]] constexpr const Http1ClientRequestBufferTooSmall* bufferTooSmall()
        const& noexcept {
        return std::get_if<Http1ClientRequestBufferTooSmall>(&state_);
    }
    const Http1ClientRequestBufferTooSmall* bufferTooSmall() const&& = delete;

    [[nodiscard]] constexpr PreparedHttp1ClientRequest* prepared() & noexcept {
        return std::get_if<PreparedHttp1ClientRequest>(&state_);
    }

    [[nodiscard]] constexpr const PreparedHttp1ClientRequest* prepared() const& noexcept {
        return std::get_if<PreparedHttp1ClientRequest>(&state_);
    }
    const PreparedHttp1ClientRequest* prepared() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientRequestPrepareFailure* failure() const& noexcept {
        return std::get_if<Http1ClientRequestPrepareFailure>(&state_);
    }
    const Http1ClientRequestPrepareFailure* failure() const&& = delete;

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    explicit constexpr Http1ClientRequestPrepareResult(
        Http1ClientRequestBufferTooSmall state) noexcept
        : state_(state) {}

    explicit Http1ClientRequestPrepareResult(PreparedHttp1ClientRequest state) noexcept
        : state_(std::move(state)) {}

    explicit constexpr Http1ClientRequestPrepareResult(
        Http1ClientRequestPrepareFailure state) noexcept
        : state_(state) {}

    std::variant<Http1ClientRequestBufferTooSmall, PreparedHttp1ClientRequest,
        Http1ClientRequestPrepareFailure>
        state_;
};

// HTTP/1.1 direct-origin request writer. Ordinary requests are allocation-free;
// an Upgrade request owns only its offered protocol value for later 101
// validation. It validates the complete request before touching the caller's
// buffer, generates Host and exact Content-Length, and returns separate
// head/content views for writev-style I/O or Expect: 100-continue gating.
// CONNECT uses its dedicated entry so authority form cannot be confused with an
// origin-form target.
class Http1ClientRequestWriter final {
public:
    struct Options final {
        std::pmr::memory_resource* resource{nullptr};
    };

    Http1ClientRequestWriter() noexcept;
    explicit Http1ClientRequestWriter(Options options) noexcept;

    [[nodiscard]] Http1ClientRequestPrepareResult prepare(const HttpOriginView& origin,
        const HttpClientRequestView& request, std::span<char> headBuffer,
        Http1ClientRequestWirePolicy policy = {}) const;

    [[nodiscard]] Http1ClientRequestPrepareResult prepareStreaming(const HttpOriginView& origin,
        const Http1ClientRequestHeadView& request, std::span<char> headBuffer,
        Http1ClientRequestWirePolicy policy = {}) const;

    [[nodiscard]] Http1ClientRequestPrepareResult prepareConnect(const HttpOriginView& tunnelOrigin,
        std::span<const HttpHeaderView> headers, std::span<char> headBuffer,
        Http1ClientRequestWirePolicy policy = {}) const;

private:
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
