#include "context/ContextServices.h"

#include <functional>

#include "server/RequestDeadline.h"

namespace ruvia::detail {

ContextServices ContextServices::withRequestDeadline(const RequestDeadline& value) const noexcept {
    auto services = *this;
    services.request_services_.stop_token = std::cref(value.token());
    services.request_services_.deadline = &value;
    return services;
}

}  // namespace ruvia::detail
