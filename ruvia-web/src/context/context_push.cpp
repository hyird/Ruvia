#include <stdexcept>
#include <utility>

#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "context/http_push_output.h"

namespace ruvia {
scoped_operation<bool> context::push(http_push_request_view request) {
    if (services().push_output() == nullptr) {
        throw std::logic_error("server push requires an HTTP/2 or HTTP/3 request");
    }
    return services().push_output()->push(request);
}
}  // namespace ruvia

namespace ruvia::detail {
scoped_operation<bool> http_push_output::push(http_push_request_view request) {
    if (scope_.has_pending_operations()) {
        throw std::logic_error("server push output is already active");
    }
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
    http_push_request owned(resource_);
    owned.method_ = request.method_;
    owned.scheme_ = request.scheme_;
    owned.authority_ = request.authority_;
    owned.path_ = request.path_;
    owned.headers_.reserve(request.headers_.size());
    for (const auto& field : request.headers_) {
        std::pmr::string name(field.name(), resource_);
        for (auto& ch : name) {
            ch = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(ch)));
        }
        owned.headers_.push_back(http_header::copy_of(name, field.value(), resource_));
    }
    return ::ruvia::make_scoped_operation(scope_, push_owned(std::move(owned)));
}
task<bool> http_push_output::push_owned(http_push_request request) {
    std::pmr::vector<http_header_view> fields(resource_);
    fields.reserve(request.headers_.size());
    for (const auto& field : request.headers_) {
        fields.emplace_back(field.name(), field.value());
    }
    co_return co_await submit_(target_, {.method_ = request.method_, .scheme_ = request.scheme_, .authority_ = request.authority_, .path_ = request.path_, .headers_ = fields});
}
}  // namespace ruvia::detail
