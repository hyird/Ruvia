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
    // HEAD must serve whatever GET serves, only without the body (RFC 9110
    // §9.3.2). Context::staticFile already builds a HEAD-correct response --
    // full-representation metadata, no Range, 304 on a matching precondition --
    // and the response writer suppresses the body for HEAD. Rejecting HEAD here
    // instead 404s a document-root file that answers 200 to GET, which breaks
    // caches, health checks, and link checkers that probe with HEAD.
    if (request.knownMethod() != HttpKnownMethod::kGet &&
        request.knownMethod() != HttpKnownMethod::kHead) {
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
