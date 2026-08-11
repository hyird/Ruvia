#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

#include "ruvia/http/Http1ClosePolicy.h"
#include "ruvia/http/HttpClient.h"
#include "ruvia/http/HttpHeader.h"
#include "ruvia/http/detail/field/HttpConnectionFields.h"

namespace ruvia {

class Http1ClientResponseParser;

// One immutable wire policy for request preparation. Expect is writer-owned so
// the emitted field and the resulting content gate cannot disagree. How long an
// I/O runtime waits before releasing a continue-gated body remains runtime
// policy, not an HTTP message-model setting.
class Http1ClientRequestWirePolicy final {
public:
    constexpr explicit Http1ClientRequestWirePolicy(Http1ClosePolicy closePolicy = Http1ClosePolicy::kAllowReuse,
        HttpClientRequestExpectation expectation = HttpClientRequestExpectation::kNone) noexcept
        : closePolicy_(closePolicy),
          expectation_(expectation) {}

    [[nodiscard]] constexpr Http1ClosePolicy closePolicy() const noexcept {
        return closePolicy_;
    }

    [[nodiscard]] constexpr HttpClientRequestExpectation expectation() const noexcept {
        return expectation_;
    }

private:
    Http1ClosePolicy closePolicy_;
    HttpClientRequestExpectation expectation_;
};

namespace detail {

struct Http1ClientRequestPrepareResultAccess;

template <typename Range>
concept HttpTemporaryOwningHeaderRange = !std::is_lvalue_reference_v<Range&&> && std::ranges::contiguous_range<Range> && !std::ranges::borrowed_range<Range> && std::same_as<std::remove_cv_t<std::ranges::range_value_t<Range>>, HttpHeaderView>;

// Exact sent-request facts needed to interpret the corresponding HTTP/1
// response. Only a successfully prepared request can create this context, so
// response framing cannot drift from the method, Upgrade offer, or close signal
// that actually entered the wire plan.
class Http1ClientRequestContext final {
public:
    [[nodiscard]] constexpr std::string_view method() const noexcept {
        return method_;
    }

    [[nodiscard]] constexpr std::span<const HttpHeaderView> headers() const noexcept {
        return headers_;
    }

    [[nodiscard]] constexpr Http1ClosePolicy closePolicy() const noexcept {
        return closePolicy_;
    }

    [[nodiscard]] constexpr HttpConnectionOptions connectionOptions() const noexcept {
        return connectionOptions_;
    }

private:
    friend struct Http1ClientRequestPrepareResultAccess;

    constexpr Http1ClientRequestContext(std::string_view method, std::span<const HttpHeaderView> headers, HttpConnectionOptions connectionOptions, Http1ClosePolicy closePolicy) noexcept
        : method_(method),
          headers_(headers),
          connectionOptions_(connectionOptions),
          closePolicy_(closePolicy) {}

    std::string_view method_;
    std::span<const HttpHeaderView> headers_;
    HttpConnectionOptions connectionOptions_;
    Http1ClosePolicy closePolicy_;
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
// The response context also borrows the request method/header table, which must
// remain alive and unchanged until the corresponding final response head or
// protocol-switch decision has been parsed.
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

private:
    friend struct detail::Http1ClientRequestPrepareResultAccess;
    friend class Http1ClientResponseParser;

    constexpr PreparedHttp1ClientRequest(std::string_view head, Http1ClientRequestContentPlan contentPlan, detail::Http1ClientRequestContext responseContext) noexcept
        : head_(head),
          contentPlan_(contentPlan),
          responseContext_(responseContext) {}

    std::string_view head_;
    Http1ClientRequestContentPlan contentPlan_;
    detail::Http1ClientRequestContext responseContext_;
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

    explicit constexpr Http1ClientRequestPrepareResult(PreparedHttp1ClientRequest state) noexcept
        : state_(state) {}

    explicit constexpr Http1ClientRequestPrepareResult(Http1ClientRequestPrepareFailure state) noexcept
        : state_(state) {}

    std::variant<Http1ClientRequestBufferTooSmall, PreparedHttp1ClientRequest, Http1ClientRequestPrepareFailure> state_;
};

// Allocation-free HTTP/1.1 direct-origin request writer. It validates the
// complete request before touching the caller's buffer, generates Host and
// exact Content-Length, and returns separate head/content views for writev-style
// I/O or Expect: 100-continue gating. CONNECT uses its dedicated entry so the
// authority-form target cannot be confused with an origin-form path.
class Http1ClientRequestWriter final {
public:
    [[nodiscard]] Http1ClientRequestPrepareResult prepare(const HttpOriginView& origin, const HttpClientRequestView& request, std::span<char> headBuffer, Http1ClientRequestWirePolicy policy = Http1ClientRequestWirePolicy()) const noexcept;

    [[nodiscard]] Http1ClientRequestPrepareResult prepareConnect(const HttpOriginView& tunnelOrigin, std::span<const HttpHeaderView> headers, std::span<char> headBuffer, Http1ClientRequestWirePolicy policy = Http1ClientRequestWirePolicy()) const noexcept;

    // The prepared response context retains the header table through the final
    // response or protocol-switch decision. A temporary owning contiguous
    // range would be destroyed as prepareConnect() returns; borrowed ranges
    // such as std::span remain valid inputs.
    template <detail::HttpTemporaryOwningHeaderRange Headers>
    Http1ClientRequestPrepareResult prepareConnect(const HttpOriginView&, Headers&&, std::span<char>, Http1ClientRequestWirePolicy = Http1ClientRequestWirePolicy()) const = delete;
};

}  // namespace ruvia
