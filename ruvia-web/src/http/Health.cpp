#include "ruvia/web/Health.h"

#include "ruvia/web/detail/json/JsonEscape.h"

#include <memory_resource>

namespace ruvia {
namespace {

[[nodiscard]] HttpResponse makeJsonResponse(
    Context& context,
    std::pmr::string& body,
    std::uint16_t statusCode = 200) {
    constexpr HttpHeaderView kJsonHeaders[] = {
        {"Content-Type", "application/json"}};
    return context.body(
        body,
        Context::ResponseInit{
            .status = statusCode,
            .headers = kJsonHeaders});
}

}  // namespace

HttpResponse makeHealthResponse(Context& context) {
    std::pmr::string body(context.resource());
    body.assign("{\"status\":\"ok\"}");
    return makeJsonResponse(context, body);
}

HttpResponse makeReadyResponse(Context& context, bool ready, std::string_view reason) {
    std::pmr::string body(context.resource());
    if (ready) {
        body.assign("{\"status\":\"ready\"}");
        return makeJsonResponse(context, body);
    }

    body.assign("{\"status\":\"not_ready\"");
    if (!reason.empty()) {
        body.append(",\"reason\":");
        detail::appendJsonString(body, reason);
    }
    body.push_back('}');
    return makeJsonResponse(context, body, 503);
}

}  // namespace ruvia
