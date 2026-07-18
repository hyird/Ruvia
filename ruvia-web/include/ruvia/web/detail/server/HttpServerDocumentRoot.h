#pragma once

#include <optional>

#include "ruvia/http/HttpResponse.h"

namespace ruvia {

class HttpRequest;
class StaticRoot;
class RequestMemory;

namespace detail {

// Serve a GET from the configured static document root, or std::nullopt when no
// root is set, the method is not GET, or the path does not resolve to a file.
// Shared by the HTTP/1 and HTTP/2 sessions so both protocols answer the same
// static URLs; without this the HTTP/2 session would 404 files that HTTP/1
// serves.
[[nodiscard]] std::optional<HttpResponse> tryStaticDocumentResponse(
    const StaticRoot* root,
    const HttpRequest& request,
    RequestMemory& memory);

}  // namespace detail
}  // namespace ruvia
