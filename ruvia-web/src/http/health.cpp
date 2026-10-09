#include "ruvia/web/health.h"

#include <stdexcept>

#include "ruvia/web/model.h"

namespace ruvia {
namespace {

RUVIA_MODEL(health_response_model, RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_OPTIONAL_FIELD(reason, ruvia::string));

}  // namespace

http_response make_health_response(context& context_value) {
    health_response_model model({.resource_ = context_value.arena()});
    model.set<"status">("ok");
    return context_value.json(model);
}

http_response make_readiness_response(context& context_value, readiness_response_options options) {
    health_response_model model({.resource_ = context_value.arena()});
    switch (options.state_) {
        case readiness_state::ready:
            model.set<"status">("ready");
            return context_value.json(model);
        case readiness_state::unavailable:
            break;
        default:
            throw std::invalid_argument("readiness state is invalid");
    }

    model.set<"status">("not_ready");
    const auto reason = options.unavailable_reason_.view();
    if (!reason.empty()) {
        model.set<"reason">(reason);
    }
    context_value.status(http_status::service_unavailable);
    return context_value.json(model);
}

}  // namespace ruvia
