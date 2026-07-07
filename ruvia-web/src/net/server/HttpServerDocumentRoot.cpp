#include "HttpServer.h"

#include "http/ContextInternal.h"
#include "ruvia/http/Error.h"

namespace ruvia::detail {

std::optional<HttpResponse> HttpServer::tryDocumentRootResponse(
    const HttpRequest& request,
    RequestMemory& memory) const {
    const auto* const root = options_.documentRoot.root;
    if (root == nullptr) {
        return std::nullopt;
    }
    if (request.method() != HttpMethod::kGet) {
        return std::nullopt;
    }

    auto relative = request.path();
    if (!relative.empty() && relative.front() == '/') {
        relative.remove_prefix(1);
    }

    auto context = ContextAccess::make(memory, request);
    try {
        return context.staticFile(*root, relative);
    } catch (const HttpError&) {
        return std::nullopt;
    }
}

}  // namespace ruvia::detail
