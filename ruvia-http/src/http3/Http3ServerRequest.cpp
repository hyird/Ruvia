#include "ruvia/http/Http3ServerRequest.h"

#include <stdexcept>
#include <string_view>

#include "parser/HttpRequestTarget.h"
#include "request/HttpRequestAccess.h"

namespace ruvia {
namespace {

std::pmr::memory_resource* normalizedResource(std::pmr::memory_resource* resource) noexcept {
    return resource != nullptr ? resource : std::pmr::get_default_resource();
}

void copyHead(Http3MessageHead& to, const Http3MessageHead& from) {
    to.method = from.method;
    to.protocol = from.protocol;
    to.scheme = from.scheme;
    to.authority = from.authority;
    to.path = from.path;
    to.status = from.status;
    to.contentLength = from.contentLength;
    to.headers.reserve(from.headers.size());
    for (const auto& field : from.headers) {
        to.headers.emplace_back(field.name, field.value, to.headers.get_allocator().resource());
    }
}

}  // namespace

Http3ServerRequest::Http3ServerRequest(const Http3MessageHead& callbackHead,
    std::pmr::memory_resource* requestResource, std::pmr::memory_resource* bodyPool)
    : head_(normalizedResource(requestResource)),
      cookies_(normalizedResource(requestResource)),
      body_(normalizedResource(bodyPool)) {
    copyHead(head_, callbackHead);
    for (const auto& field : head_.headers) {
        if (field.name == "expect") {
            expectations_.parseField(field.value);
        }
    }
    buildRequest();
}

void Http3ServerRequest::buildRequest() {
    const bool standardConnect = head_.method == "CONNECT" && head_.protocol.empty();
    const std::string_view target = standardConnect ? std::string_view(head_.authority)
                                                    : std::string_view(head_.path);
    const auto queryAt = standardConnect ? std::string_view::npos : target.find('?');
    const auto path = standardConnect ? std::string_view{} : target.substr(0, queryAt);
    const auto query = queryAt == std::string_view::npos ? std::string_view{}
                                                         : target.substr(queryAt + 1);

    bool hasHost = false;
    std::size_t cookieCount = 0;
    for (const auto& field : head_.headers) {
        if (field.name == "host") {
            hasHost = true;
        }
        if (field.name == "cookie") {
            if (cookieCount != 0) {
                cookies_.append("; ");
            }
            cookies_.append(field.value);
            ++cookieCount;
        }
    }
    const bool synthesizeHost = !hasHost && !head_.authority.empty() &&
                                detail::isValidHostHeader(head_.authority);
    const std::size_t outputCount = head_.headers.size() - cookieCount +
                                    static_cast<std::size_t>(cookieCount != 0) +
                                    static_cast<std::size_t>(synthesizeHost);
    if (outputCount > kMaxHttpHeaderFields) {
        throw std::length_error("too many HTTP/3 request headers");
    }

    detail::HttpRequestAccess::setResource(request_, normalizedResource(head_.method.get_allocator().resource()));
    detail::HttpRequestAccess::setMethod(request_, head_.method);
    detail::HttpRequestAccess::setProtocolVersion(request_, HttpProtocolVersion::kHttp3);
    detail::HttpRequestAccess::setTarget(request_, target);
    detail::HttpRequestAccess::setScheme(request_, head_.scheme);
    detail::HttpRequestAccess::setAuthority(request_, head_.authority);
    detail::HttpRequestAccess::setTargetForm(request_, HttpRequestTargetForm::kHttp3);
    detail::HttpRequestAccess::setPath(request_, path);
    detail::HttpRequestAccess::setQueryString(request_, query);
    detail::HttpRequestAccess::reserveHeaders(request_, outputCount);

    bool emittedCookie = false;
    for (const auto& field : head_.headers) {
        if (field.name == "cookie") {
            if (!emittedCookie) {
                detail::HttpRequestAccess::addHeader(request_, HttpHeaderView("cookie", cookies_));
                emittedCookie = true;
            }
            continue;
        }
        detail::HttpRequestAccess::addHeader(request_, HttpHeaderView(field.name, field.value));
    }
    if (synthesizeHost) {
        detail::HttpRequestAccess::addHeader(request_, HttpHeaderView("host", head_.authority));
    }
}

void Http3ServerRequest::appendBody(std::span<const std::byte> bytes) {
    if (bodyComplete_ || bodyAborted_) {
        throw std::logic_error("HTTP/3 request body is terminal");
    }
    body_.insert(body_.end(), bytes.begin(), bytes.end());
}

void Http3ServerRequest::finishBody() {
    if (bodyComplete_ || bodyAborted_) {
        throw std::logic_error("HTTP/3 request body is terminal");
    }
    detail::HttpRequestAccess::setBody(request_, std::span<const std::byte>(body_));
    bodyComplete_ = true;
}

void Http3ServerRequest::abortBody() noexcept {
    if (bodyComplete_ || bodyAborted_) {
        return;
    }
    std::pmr::vector<std::byte> empty(body_.get_allocator().resource());
    body_.swap(empty);
    bodyAborted_ = true;
}

}  // namespace ruvia
