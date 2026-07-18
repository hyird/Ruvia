#include "ruvia/web/detail/server/HttpServerDocumentRoot.h"

#include "ruvia/web/detail/server/HttpServer.h"

#include "ruvia/web/detail/http/ContextInternal.h"
#include "ruvia/web/Error.h"

namespace ruvia::detail {

std::optional<HttpResponse> tryStaticDocumentResponse(
    const StaticRoot* const root,
    const HttpRequest& request,
    RequestMemory& memory) {
    if (root == nullptr) {
        return std::nullopt;
    }
    if (request.knownMethod() != HttpKnownMethod::kGet) {
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

std::optional<HttpResponse> HttpServer::tryDocumentRootResponse(
    const HttpRequest& request,
    RequestMemory& memory) const {
    return tryStaticDocumentResponse(
        options_.documentRoot.root, request, memory);
}

}  // namespace ruvia::detail
