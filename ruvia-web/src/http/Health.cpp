#include "ruvia/web/Health.h"

#include "ruvia/web/Model.h"

namespace ruvia {
namespace {

RUVIA_RESPONSE_MODEL(HealthResponseModel,
    RUVIA_REQUIRED_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(reason, ruvia::String));

}  // namespace

HttpResponse makeHealthResponse(Context& context) {
    HealthResponseModel model(context.resource());
    model.set<"status">("ok");
    return context.json(model);
}

HttpResponse makeReadyResponse(Context& context, bool ready, std::string_view reason) {
    HealthResponseModel model(context.resource());
    if (ready) {
        model.set<"status">("ready");
        return context.json(model);
    }

    model.set<"status">("not_ready");
    if (!reason.empty()) {
        model.set<"reason">(reason);
    }
    context.status(http_status::kServiceUnavailable);
    return context.json(model);
}

}  // namespace ruvia
