#include "context/context_services.h"

#include <functional>

#include "http/streaming_access.h"
#include "server/request_deadline.h"

namespace ruvia::detail {

bool context_response_output::committed() const noexcept {
    if (buffered() != nullptr) {
        return false;
    }
    if (const auto* stream = response_stream()) {
        return streaming_access::committed(stream->writer());
    }
    return true;
}

context_services context_services::with_request_deadline(
    const ::ruvia::detail::request_deadline& value) const noexcept {
    auto services = *this;
    services.request_services_.stop_token_ = std::cref(value.token());
    services.request_services_.deadline_ = &value;
    return services;
}

}  // namespace ruvia::detail
