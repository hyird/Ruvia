#include "ruvia/web/context.h"
#include "ruvia/web/dotenv.h"
#include "ruvia/web/streaming.h"

#include "context/context_request_storage.h"
#include "context/context_services.h"
#include "router/route_limits.h"
#include "server/forwarded_headers.h"

#ifdef RUVIA_ENABLE_DATABASE
#include "ruvia/web/db/db_handle.h"
#endif

#ifdef RUVIA_ENABLE_REDIS
#include "ruvia/web/redis/redis_handle.h"
#endif
namespace ruvia {

context::context(
    request_memory& memory, const http_request& request, const detail::context_services& services)
    : context(memory, request, {}, nullptr, nullptr, 0, 0, services) {}

context::context(request_memory& memory, const http_request& request,
    std::string_view route_path, const std::string_view* param_names,
    const std::string_view* param_values, std::size_t param_count, std::uintptr_t route_rate_limit_scope,
    const detail::context_services& services)
    : memory_(memory),
      request_(request),
      conn_info_(services.get_conn_info()),
      capabilities_(services.worker(), services.get_stop_token(), services.worker_states()),
      route_path_(route_path),
      param_names_(param_names),
      param_values_(param_values),
      param_count_(param_count < detail::max_route_params ? param_count : detail::max_route_params),
      route_rate_limit_scope_(route_rate_limit_scope),
      request_storage_(make_pmr_object<detail::context_request_storage>(memory.resource(),
          services, memory.resource())) {}

context::~context() = default;

bool context::is_subrequest() const noexcept {
    return services().dispatch_depth() != 0;
}

::ruvia::stop_token context::drain_token() const noexcept {
    return services().drain_token();
}

detail::context_services& context::services() noexcept {
    return request_storage().services_;
}

const detail::context_services& context::services() const noexcept {
    return request_storage().services_;
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

const http_response* context::response() const noexcept {
    const auto* final = response_state().final();
    return final;
}

bool context::has_response() const noexcept {
    return response_state().final() != nullptr;
}

void context::respond(http_response&& response) {
    store_response(std::move(response));
}

#ifdef RUVIA_ENABLE_DATABASE
db_handle context::db() const {
    const auto& services_value = services();
    return services_value.client_registries().db(
        operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}

db_handle context::db(std::string_view alias) const {
    const auto& services_value = services();
    return services_value.client_registries().db(
        alias, operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}
#endif

#ifdef RUVIA_ENABLE_REDIS
redis_handle context::redis() const {
    const auto& services_value = services();
    return services_value.client_registries().redis(
        operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}

redis_handle context::redis(std::string_view alias) const {
    const auto& services_value = services();
    return services_value.client_registries().redis(
        alias, operation_scope_, services_value.io_metrics(), capabilities_.stop_token());
}
#endif

bool detail::context_response_output::committed() const noexcept {
    if (buffered() != nullptr) {
        return false;
    }
    if (const auto* stream = response_stream()) {
        return stream->writer().committed();
    }
    return true;
}

conn_info detail::context_services::resolve_conn_info(const http_request& request) const noexcept {
    auto resolved = connection_services_.info_;
    if (!connection_services_.peer_trusted_) {
        return resolved;
    }
    const auto forwarded = detail::resolve_forwarded_client(request, *worker_services_.trusted_proxies_);
    resolved.apply_forwarded(forwarded.address_, forwarded.scheme_);
    return resolved;
}

}  // namespace ruvia
