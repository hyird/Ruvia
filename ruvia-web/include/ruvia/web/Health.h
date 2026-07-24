#pragma once

#include <string_view>

#include "ruvia/web/Context.h"
#include "ruvia/http/HttpResponse.h"

namespace ruvia {

[[nodiscard]] HttpResponse makeHealthResponse(Context& context);

[[nodiscard]] HttpResponse makeReadyResponse(Context& context, bool ready, std::string_view reason = "service is not ready");

}  // namespace ruvia
