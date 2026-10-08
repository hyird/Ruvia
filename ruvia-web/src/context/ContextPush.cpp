#include <stdexcept>
#include <utility>

#include "ruvia/http/HttpAscii.h"
#include "ruvia/http/HttpLimits.h"
#include "ruvia/web/Context.h"

#include "context/ContextServices.h"
#include "context/HttpPushOutput.h"

namespace ruvia {
ScopedOperation<bool> Context::push(HttpPushRequestView request) {
    if (services().pushOutput() == nullptr) {
        throw std::logic_error("server push requires an HTTP/2 or HTTP/3 request");
    }
    return services().pushOutput()->push(request);
}
}  // namespace ruvia

namespace ruvia::detail {
ScopedOperation<bool> HttpPushOutput::push(HttpPushRequestView request) {
    if (scope_.has_pending_operations()) {
        throw std::logic_error("server push output is already active");
    }
    std::size_t bytes = 0;
    const auto count = [&bytes](std::string_view value) {
        if (value.size() > kMaxHttpHeaderBytes - bytes) {
            throw std::length_error("push request exceeds field byte bound");
        }
        bytes += value.size();
    };
    count(request.method);
    count(request.scheme);
    count(request.authority);
    count(request.path);
    if (request.headers.size() > kMaxHttpHeaderFields - 4) {
        throw std::length_error("push request exceeds field count bound");
    }
    for (const auto& field : request.headers) {
        count(field.name());
        count(field.value());
        count(std::string_view("................................"));
    }
    HttpPushRequest owned(resource_);
    owned.method = request.method;
    owned.scheme = request.scheme;
    owned.authority = request.authority;
    owned.path = request.path;
    owned.headers.reserve(request.headers.size());
    for (const auto& field : request.headers) {
        std::pmr::string name(field.name(), resource_);
        for (auto& ch : name) {
            ch = static_cast<char>(httpAsciiToLower(static_cast<unsigned char>(ch)));
        }
        owned.headers.push_back(HttpHeader::copyOf(name, field.value(), resource_));
    }
    return ::ruvia::make_scoped_operation(scope_, pushOwned(std::move(owned)));
}
Task<bool> HttpPushOutput::pushOwned(HttpPushRequest request) {
    std::pmr::vector<HttpHeaderView> fields(resource_);
    fields.reserve(request.headers.size());
    for (const auto& field : request.headers) {
        fields.emplace_back(field.name(), field.value());
    }
    co_return co_await submit_(target_, {.method = request.method, .scheme = request.scheme, .authority = request.authority, .path = request.path, .headers = fields});
}
}  // namespace ruvia::detail
