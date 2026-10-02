#include <stdexcept>
#include <utility>
#include <vector>

#include "ruvia/web/Context.h"
#include "ruvia/web/detail/http/context/HttpInterimResponseOutput.h"

namespace ruvia {
ScopedOperation<void> Context::inform(const HttpInterimResponseHead& response) {
    if (interimOutput_ == nullptr) {
        throw std::logic_error("this dispatch has no transport response output");
    }
    return interimOutput_->inform(response);
}
}  // namespace ruvia

namespace ruvia::detail {
ScopedOperation<void> HttpInterimResponseOutput::inform(const HttpInterimResponseHead& response) {
    if (finalCommitted_) {
        throw std::logic_error("the final response has already started");
    }
    if (scope_.hasPendingOperations()) {
        throw std::logic_error("interim response output is already active");
    }
    std::pmr::vector<HttpHeader> owned(resource_);
    owned.reserve(response.headers().size());
    for (const auto& field : response.headers()) {
        owned.push_back(HttpHeader::copyOf(field.name(), field.value(), resource_));
    }
    return makeScopedOperation(scope_, writeOwned(response.status(), std::move(owned)));
}
Task<void> HttpInterimResponseOutput::writeOwned(HttpStatusCode status, std::pmr::vector<HttpHeader> fields) {
    if (finalCommitted_) {
        throw std::logic_error("the final response has already started");
    }
    std::pmr::vector<HttpHeaderView> views(resource_);
    views.reserve(fields.size());
    for (const auto& field : fields) {
        views.emplace_back(field.name(), field.value());
    }
    const HttpInterimResponseHead head(status, std::span<const HttpHeaderView>(views));
    co_await write_(target_, head);
}
}  // namespace ruvia::detail
