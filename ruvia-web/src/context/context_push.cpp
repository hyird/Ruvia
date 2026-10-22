#include <stdexcept>

#include "ruvia/http/http_limits.h"
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

namespace ruvia::detail {
bool http_push_output::push(http_push_request_view request) {
    std::size_t bytes_value = 0;
    const auto count = [&bytes_value](std::string_view value) {
        if (value.size() > max_http_header_bytes - bytes_value) {
            throw std::length_error("push request exceeds field byte bound");
        }
        bytes_value += value.size();
    };
    count(request.method_);
    count(request.scheme_);
    count(request.authority_);
    count(request.path_);
    if (request.headers_.size() > max_http_header_fields - 4) {
        throw std::length_error("push request exceeds field count bound");
    }
    for (const auto& field : request.headers_) {
        count(field.name());
        count(field.value());
        count(std::string_view("................................"));
    }
    return submit_(target_, request);
}
}  // namespace ruvia::detail
