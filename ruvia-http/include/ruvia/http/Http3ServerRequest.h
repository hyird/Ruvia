#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/http/Attributes.h"
#include "ruvia/http/Http3MessageHead.h"
#include "ruvia/http/HttpExpectations.h"
#include "ruvia/http/HttpKnownMethod.h"
#include "ruvia/http/HttpRequest.h"

namespace ruvia {

// Owns a decoded HTTP/3 request head and body for the lifetime of its borrowed
// HttpRequest view. requestResource and bodyPool must outlive this owner.
class Http3ServerRequest final {
public:
    // Copies all head data before returning. The body is accumulated separately
    // and is not published through request() until finishBody().
    Http3ServerRequest(const Http3MessageHead& callbackHead,
        std::pmr::memory_resource* requestResource, std::pmr::memory_resource* bodyPool);

    Http3ServerRequest(const Http3ServerRequest&) = delete;
    Http3ServerRequest& operator=(const Http3ServerRequest&) = delete;
    Http3ServerRequest(Http3ServerRequest&&) = delete;
    Http3ServerRequest& operator=(Http3ServerRequest&&) = delete;

    [[nodiscard]] const HttpRequest& request() const& noexcept {
        return request_;
    }
    const HttpRequest& request() const&& = delete;

    [[nodiscard]] std::string_view extendedConnectProtocol() const& noexcept RUVIA_LIFETIMEBOUND {
        return head_.protocol;
    }
    std::string_view extendedConnectProtocol() const&& = delete;

    [[nodiscard]] HttpServerExpectationPlan expectationPlan(HttpUnsupportedExpectationPolicy policy) const noexcept {
        const bool contentRemaining = !bodyComplete_ && !bodyAborted_ && request_.knownMethod() != HttpKnownMethod::kConnect &&
                                      (!head_.contentLength || *head_.contentLength > body_.size());
        return expectations_.serverPlan(contentRemaining ? HttpRequestContentIndication::kWillFollow : HttpRequestContentIndication::kNoContent, policy);
    }

    [[nodiscard]] bool bodyComplete() const noexcept {
        return bodyComplete_;
    }
    [[nodiscard]] std::size_t bodyBytes() const noexcept {
        return body_.size();
    }
    void appendBody(std::span<const std::byte> bytes);
    void finishBody();
    void abortBody() noexcept;

private:
    void buildRequest();

    // request_ is destroyed before the storage its views borrow.
    Http3MessageHead head_;
    std::pmr::string cookies_;
    std::pmr::vector<std::byte> body_;
    HttpRequest request_;
    HttpRequestExpectations expectations_{};
    bool bodyComplete_{false};
    bool bodyAborted_{false};
};

}  // namespace ruvia
