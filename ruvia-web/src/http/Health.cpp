#include "ruvia/web/Health.h"

#include "ruvia/web/Model.h"

namespace ruvia {
namespace {

RUVIA_RESPONSE_MODEL(HealthResponseModel,
    RUVIA_REQUIRED_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(reason, ruvia::String));

}  // namespace

HttpResponse makeHealthResponse(Context& context) {
    HealthResponseModel model({.resource = context.resource()});
    model.set<"status">("ok");
    return context.json(model);
}

HttpResponse makeReadinessResponse(Context& context, ReadinessResponseOptions options) {
    HealthResponseModel model({.resource = context.resource()});
    if (options.state == ReadinessState::kReady) {
        model.set<"status">("ready");
        return context.json(model);
    }

    model.set<"status">("not_ready");
    const auto reason = options.unavailableReason.view();
    if (!reason.empty()) {
        model.set<"reason">(reason);
    }
    context.status(http_status::kServiceUnavailable);
    return context.json(model);
}

}  // namespace ruvia
