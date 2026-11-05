#include <stdexcept>

#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "context/http_push_output.h"

namespace ruvia {
bool context::push(http_push_request_view request) {
    if (services().push_output() == nullptr) {
        throw std::logic_error("server push requires an HTTP/2 request");
    }
    return services().push_output()->push(request);
}
}  // namespace ruvia
