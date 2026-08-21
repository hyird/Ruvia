#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <variant>

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
struct Http1ClientExchangeStateAccess;

enum class Http1ClientInitialContentState : std::uint8_t {
    kComplete,
    kPending,
    kAwaitingContinue,
};

}  // namespace detail

// Owning protocol facts for exactly one HTTP/1 response exchange. Preparing a
// request creates this state from the facts that actually entered the wire
// plan; transferring it to the response parser removes every dependency on the
// caller's method and header storage. Only an offered Upgrade value needs owned
// dynamic storage, so ordinary requests remain allocation-free.
class Http1ClientExchangeState final {
public:
    Http1ClientExchangeState(const Http1ClientExchangeState&) = delete;
    Http1ClientExchangeState& operator=(const Http1ClientExchangeState&) = delete;
    Http1ClientExchangeState(Http1ClientExchangeState&&) noexcept = default;
    Http1ClientExchangeState& operator=(Http1ClientExchangeState&&) = delete;

private:
    friend class PreparedHttp1ClientRequest;
    friend struct detail::Http1ClientRequestPrepareResultAccess;
    friend struct detail::Http1ClientExchangeStateAccess;

    Http1ClientExchangeState(const Http1ClientExchangeState& other, std::pmr::memory_resource* resource)
        : offeredUpgradeProtocols_(other.offeredUpgradeProtocols_, resource),
          method_(other.method_),
          connectionOptions_(other.connectionOptions_),
          closePolicy_(other.closePolicy_),
          contentState_(other.contentState_) {}

    Http1ClientExchangeState(HttpKnownMethod method, detail::HttpConnectionOptions connectionOptions,
        Http1ClosePolicy closePolicy, detail::Http1ClientInitialContentState contentState,
        std::pmr::string offeredUpgradeProtocols) noexcept
        : offeredUpgradeProtocols_(std::move(offeredUpgradeProtocols)),
          method_(method),
          connectionOptions_(connectionOptions),
          closePolicy_(closePolicy),
          contentState_(contentState) {}

    std::pmr::string offeredUpgradeProtocols_;
    HttpKnownMethod method_{HttpKnownMethod::kUnknown};
    detail::HttpConnectionOptions connectionOptions_;
    Http1ClosePolicy closePolicy_{Http1ClosePolicy::kAllowReuse};
    detail::Http1ClientInitialContentState contentState_{detail::Http1ClientInitialContentState::kComplete};
};

namespace detail {

struct Http1ClientExchangeStateAccess final {
    [[nodiscard]] static constexpr HttpKnownMethod method(const Http1ClientExchangeState& state) noexcept {
        return state.method_;
    }

    [[nodiscard]] static constexpr HttpConnectionOptions connectionOptions(const Http1ClientExchangeState& state) noexcept {
        return state.connectionOptions_;
    }

    [[nodiscard]] static constexpr Http1ClosePolicy closePolicy(const Http1ClientExchangeState& state) noexcept {
        return state.closePolicy_;
    }

    [[nodiscard]] static constexpr Http1ClientInitialContentState contentState(const Http1ClientExchangeState& state) noexcept {
        return state.contentState_;
    }

    [[nodiscard]] static std::string_view offeredUpgradeProtocols(const Http1ClientExchangeState& state) noexcept {
        return state.offeredUpgradeProtocols_;
    }
};

}  // namespace detail

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
    [[nodiscard]] constexpr const Http1ClientRequestWithoutContent* withoutContent() const& noexcept {
        return std::get_if<Http1ClientRequestWithoutContent>(&content_);
    }
    const Http1ClientRequestWithoutContent* withoutContent() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientImmediateRequestContent* immediate() const& noexcept {
        return std::get_if<Http1ClientImmediateRequestContent>(&content_);
    }
    const Http1ClientImmediateRequestContent* immediate() const&& = delete;

    [[nodiscard]] constexpr const Http1ClientContinueGatedRequestContent* continueGated() const& noexcept {
        return std::get_if<Http1ClientContinueGatedRequestContent>(&content_);
    }
    const Http1ClientContinueGatedRequestContent* continueGated() const&& = delete;

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    using Content = std::variant<Http1ClientRequestWithoutContent, Http1ClientImmediateRequestContent, Http1ClientContinueGatedRequestContent>;

    explicit constexpr Http1ClientRequestContentPlan(Http1ClientRequestWithoutContent content) noexcept
        : content_(content) {}

    explicit constexpr Http1ClientRequestContentPlan(Http1ClientImmediateRequestContent content) noexcept
        : content_(content) {}

    explicit constexpr Http1ClientRequestContentPlan(Http1ClientContinueGatedRequestContent content) noexcept
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
};

[[nodiscard]] std::string_view http1ClientRequestPrepareErrorMessage(Http1ClientRequestPrepareError error) noexcept;

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
        return Http1ClientExchangeState(exchangeState_, exchangeState_.offeredUpgradeProtocols_.get_allocator().resource());
    }
    Http1ClientExchangeState exchangeState() const&& = delete;

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;

    PreparedHttp1ClientRequest(std::string_view head, Http1ClientRequestContentPlan contentPlan, Http1ClientExchangeState exchangeState) noexcept
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

    explicit constexpr Http1ClientRequestPrepareFailure(Http1ClientRequestPrepareError error) noexcept
        : error_(error) {}

    Http1ClientRequestPrepareError error_;
};

class Http1ClientRequestPrepareResult final {
public:
    [[nodiscard]] constexpr const Http1ClientRequestBufferTooSmall* bufferTooSmall() const& noexcept {
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

    explicit constexpr Http1ClientRequestPrepareResult(Http1ClientRequestBufferTooSmall state) noexcept
        : state_(state) {}

    explicit Http1ClientRequestPrepareResult(PreparedHttp1ClientRequest state) noexcept
        : state_(std::move(state)) {}

    explicit constexpr Http1ClientRequestPrepareResult(Http1ClientRequestPrepareFailure state) noexcept
        : state_(state) {}

    std::variant<Http1ClientRequestBufferTooSmall, PreparedHttp1ClientRequest, Http1ClientRequestPrepareFailure> state_;
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

    explicit Http1ClientRequestWriter(Options options = {}) noexcept;

    [[nodiscard]] Http1ClientRequestPrepareResult prepare(const HttpOriginView& origin, const HttpClientRequestView& request, std::span<char> headBuffer, Http1ClientRequestWirePolicy policy = {}) const;

    [[nodiscard]] Http1ClientRequestPrepareResult prepareConnect(const HttpOriginView& tunnelOrigin, std::span<const HttpHeaderView> headers, std::span<char> headBuffer, Http1ClientRequestWirePolicy policy = {}) const;

private:
    std::pmr::memory_resource* resource_;
};

}  // namespace ruvia
