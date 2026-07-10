#include "ruvia/web/Health.h"

#include "ruvia/http/detail/HttpResponseBodyAccess.h"
#include "ruvia/http/JsonUtils.h"

#include <memory_resource>

namespace ruvia {
namespace {

[[nodiscard]] HttpResponse makeJsonResponse(
    Context& context,
    std::pmr::string body,
    std::uint16_t statusCode = 200) {
    HttpResponse response(context.resource());
    response.status(statusCode, {});
    response.header("Content-Type", "application/json");
    detail::setResponseBodyOwned(response, std::move(body));
    return response;
}

}  // namespace

HttpResponse makeHealthResponse(Context& context) {
    std::pmr::string body(context.resource());
    body.assign("{\"status\":\"ok\"}");
    return makeJsonResponse(context, std::move(body));
}

HttpResponse makeReadyResponse(Context& context, bool ready, std::string_view reason) {
    std::pmr::string body(context.resource());
    if (ready) {
        body.assign("{\"status\":\"ready\"}");
        return makeJsonResponse(context, std::move(body));
    }

    body.assign("{\"status\":\"not_ready\"");
    if (!reason.empty()) {
        body.append(",\"reason\":");
        detail::appendJsonString(body, reason);
    }
    body.push_back('}');
    return makeJsonResponse(context, std::move(body), 503);
}

}  // namespace ruvia
