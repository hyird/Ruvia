#include "ruvia/web/context.h"
#include "ruvia/web/dotenv.h"

#include "context/context_request_storage.h"
#include "context/context_services.h"
#include "router/route_limits.h"
#include "server/request_deadline.h"

namespace ruvia {

context::context(
    request_memory& memory, const http_request& request, detail::context_services services)
    : context(memory, request, {}, nullptr, nullptr, 0, 0, services) {}

context::context(request_memory& memory, const http_request& request,
    std::string_view route_path, const std::string_view* param_names,
    const std::string_view* param_values, std::size_t param_count, std::uintptr_t route_rate_limit_scope,
    detail::context_services services)
    : memory_(memory),
      request_(request),
      // A subrequest's services carry the parent's snapshot, so replay risk
      // survives internal dispatch even without a forwarded Early-Data field.
      early_data_info_(services.early_data_info().received_from_early_data(),
          services.early_data_info().upstream_declared_early_data() || request.header("early-data").has_value()),
      conn_info_(services.resolve_conn_info(request)),
      capabilities_(services.worker(), services.get_stop_token(), services.worker_states(), services.get_blocking_pool()),
      route_path_(route_path),
      param_names_(param_names),
      param_values_(param_values),
      param_count_(param_count < detail::max_route_params ? param_count : detail::max_route_params),
      route_rate_limit_scope_(route_rate_limit_scope),
      request_storage_(detail::make_pmr_object<detail::context_request_storage>(memory.resource(),
          services, memory.resource())) {
    if (!services.automatic_alt_svc().empty()) {
        set_stable_response_header("Alt-Svc", services.automatic_alt_svc());
    }
}

context::~context() = default;

bool context::is_subrequest() const noexcept {
    return services().dispatch_depth() != 0;
}

detail::context_services& context::services() noexcept {
    return request_storage().services_;
}

const detail::context_services& context::services() const noexcept {
    return request_storage().services_;
}

bool context::deadline_exceeded() const noexcept {
    return services().request_deadline() != nullptr && services().request_deadline()->exceeded();
}

const ruvia::env& context::env() const noexcept {
    static const ruvia::env empty;
    return services().env() != nullptr ? *services().env() : empty;
}

std::pmr::string& context::decoded_body() const {
    auto& storage = request_storage();
    if (!storage.decoded_body_) {
        storage.decoded_body_.emplace(services().inbound_buffer_pool() != nullptr ? services().inbound_buffer_pool() : pool());
    }
    return *storage.decoded_body_;
}

detail::context_request_storage& context::request_storage() const {
    return *request_storage_;
}

detail::context_request_body_source& context::request_body_source() noexcept {
    return services().request_body_source();
}

const detail::context_request_body_source& context::request_body_source() const noexcept {
    return services().request_body_source();
}

detail::context_response_output& context::response_output() noexcept {
    return services().response_output();
}

const detail::context_response_output& context::response_output() const noexcept {
    return services().response_output();
}

detail::context_response_state& context::response_state() noexcept {
    return request_storage().response_state_;
}

const detail::context_response_state& context::response_state() const noexcept {
    return request_storage().response_state_;
}

detail::context_session_state& context::session_state() noexcept {
    return request_storage().session_state_;
}

const detail::context_session_state& context::session_state() const noexcept {
    return request_storage().session_state_;
}

detail::request_bindings& context::request_bindings() noexcept {
    return request_storage().request_bindings_;
}

const detail::request_bindings& context::request_bindings() const noexcept {
    return request_storage().request_bindings_;
}

http_response& context::response_storage() {
    return response_state().materialize_provisional();
}

const http_response* context::response() const noexcept {
    const auto* final = response_state().final();
    return final == nullptr ? nullptr : &final->response();
}

bool context::has_response() const noexcept {
    return response_state().final() != nullptr;
}

void context::respond(http_response&& response) {
    store_assigned_response(std::move(response));
}

http_response context::take_response() {
    return response_state().take();
}

}  // namespace ruvia
