#include <array>
#include <stdexcept>
#include <variant>

#include "ruvia/http/http_request_trailers.h"
#include "ruvia/web/context.h"

#include "context/context_services.h"
#include "http/request_query_values.h"

namespace ruvia {

task<std::string_view> context_request::context_text_task(const context* context_value) {
    return context_value->request_body();
}

bool context_request::context_content_type_matches(
    const context* context_value, std::string_view expected) noexcept {
    return context_value->request_content_type_matches(expected);
}

std::pmr::memory_resource* context_request::context_resource(const context* context_value) noexcept {
    return context_value->arena();
}

::ruvia::operation_scope& context_request::context_operation_scope(
    const context* context_value) noexcept {
    return context_value->operation_scope_;
}

std::string_view context_request::method() const noexcept {
    return context_->request_.method();
}

http_known_method context_request::known_method() const noexcept {
    return context_->request_.known_method();
}

std::string_view context_request::path() const noexcept {
    return context_->request_.path();
}

std::string_view context_request::scheme() const noexcept {
    return context_->request_.scheme();
}
std::string_view context_request::authority() const noexcept {
    return context_->request_.authority();
}
std::string_view context_request::target() const noexcept {
    return context_->request_.target();
}
http_protocol_version context_request::protocol_version() const noexcept {
    return context_->request_.protocol_version();
}
http_request_target_form context_request::target_form() const noexcept {
    return context_->request_.target_form();
}

std::string_view context_request::route_path() const noexcept {
    return context_->route_path_;
}
http_priority context_request::priority() const noexcept {
    if (context_->services().request_priority_update() != nullptr && *context_->services().request_priority_update()) {
        return **context_->services().request_priority_update();
    }
    const auto parsed_value = parse_http_priority(context_->request_.headers());
    return (parsed_value.index() == 0) ? std::get<0>(parsed_value).request_priority() : http_priority{};
}
void context::priority(http_priority_fields fields_value) {
    std::array<char, 12> value{};
    const auto encoded = encode_http_priority(value, fields_value);
    if ((encoded.index() != 0)) {
        throw std::invalid_argument("invalid HTTP priority parameters");
    }
    header("Priority", std::string_view(value.data(), std::get<0>(encoded)));
}

std::optional<std::string_view> context_request::header(std::string_view name) const {
    return context_->request_header(name);
}
std::span<const http_header> context_request::trailers() const noexcept {
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->fields() : std::span<const http_header>{};
}
std::optional<std::string_view> context_request::trailer(std::string_view name) const noexcept {
    return context_->services().request_trailers() != nullptr ? context_->services().request_trailers()->field(name) : std::nullopt;
}

bool context_request::accepts(std::string_view media_type) const noexcept {
    return context_->request_accepts(media_type);
}

std::optional<std::string_view> context_request::negotiate(
    negotiable_type field, std::span<const std::string_view> supported) const noexcept {
    return context_->request_negotiate(field, supported);
}

std::optional<std::string_view> context_request::query(std::string_view name) const {
    return context_->request_query(name);
}

std::span<const std::string_view> context_request::queries(std::string_view name) const {
    return context_->request_queries().values(name);
}

std::optional<std::string_view> context_request::cookie(std::string_view name) const {
    return context_->request_cookie(name);
}

const request_name_value_list& context_request::header_fields() const {
    return context_->request_headers();
}

const request_name_value_list& context_request::query_fields() const {
    return context_->request_query();
}

const request_name_value_list& context_request::cookie_fields() const {
    return context_->request_cookies();
}

const request_name_value_list& context_request::param_fields() const {
    return context_->route_params();
}

scoped_operation<std::string_view> context_request::text() const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, context_->request_body());
}

task<std::span<const std::byte>> context_request::bytes_task(const context* context_value) {
    const auto body = co_await context_text_task(context_value);
    co_return std::as_bytes(std::span(body));
}

scoped_operation<std::span<const std::byte>> context_request::bytes() const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, bytes_task(context_));
}

task<context_request::request_blob_type> context_request::blob_task(const context* context_value) {
    auto bytes_value = co_await bytes_task(context_value);
    co_return request_blob_type(
        bytes_value, context_value->request_header("Content-Type").value_or(std::string_view{}));
}

scoped_operation<context_request::request_blob_type> context_request::blob() const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, blob_task(context_));
}

scoped_operation<void> context_request::discard_body() const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, context_->request_discard_body());
}

scoped_operation<std::pmr::vector<multipart_part>> context_request::multipart() const {
    return ::ruvia::make_scoped_operation(context_->operation_scope_, context_->request_multipart());
}

body_reader& context_request::get_body_reader() const {
    return context_->request_body_reader();
}

multipart_reader context_request::get_multipart_reader() const {
    return context_->request_multipart_reader();
}

std::optional<std::string_view> context_request::param(std::string_view name) const {
    return context_->route_param(name);
}

bool context_request::content_type_matches(std::string_view expected) const noexcept {
    return context_->request_content_type_matches(expected);
}

std::pmr::memory_resource* context_request::resource() const noexcept {
    return context_->arena();
}

const detail::request_bindings& context_request::request_bindings() const noexcept {
    return context_->request_bindings();
}

}  // namespace ruvia
