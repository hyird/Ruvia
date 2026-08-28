#include "ruvia/web/detail/http/context/ContextServices.h"

#include <functional>

#include "ruvia/web/detail/server/RequestDeadline.h"

namespace ruvia::detail {

ContextServices ContextServices::withRequestDeadline(const RequestDeadline& value) const noexcept {
    auto services = *this;
    services.stopToken_ = std::cref(value.token());
    services.requestDeadline_ = &value;
    return services;
}

}  // namespace ruvia::detail
