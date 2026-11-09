#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_field_values.h"
#include "ruvia/http/http_request.h"
#include "ruvia/web/context.h"
#include "ruvia/web/dotenv.h"
#include "ruvia/web/streaming.h"

#include "context/context_request_storage.h"
#include "context/context_services.h"
#include "router/route_resolution.h"
#include "server/trusted_proxies.h"

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
      capabilities_(services.worker(), services.get_stop_token(), services.worker_states()),
      route_path_(route_path),
      param_names_(param_names),
      param_values_(param_values),
      param_count_(param_count < detail::max_route_params ? param_count : detail::max_route_params),
      route_rate_limit_scope_(route_rate_limit_scope),
      request_storage_(make_pmr_object<detail::context_request_storage>(memory.resource(),
          services, memory.resource())) {
    operation_scope_.bind_worker(services.worker());
}
context::~context() = default;

const conn_info& context::conn() const& noexcept {
    return services().get_conn_info();
}

bool context::is_subrequest() const noexcept {
    return services().dispatch_depth() != 0;
}

bool context::early_data() const noexcept {
    return services().early_data();
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

namespace detail {

// Reading the client's address and scheme out of forwarding headers, for a
// request whose peer the deployment has already declared trusted.
//
// Reverse proxies rewrite X-Forwarded-For / X-Forwarded-Proto and commonly
// forward a client `Forwarded` field unchanged. Preferring RFC 7239 when both
// are present would let the caller pick conn_info while the proxy-written
// X- headers name the real hop. X-Forwarded-* therefore win when present;
// `Forwarded` is the fallback when the proxy speaks only RFC 7239.
//
// Same-name list fields are one chain (RFC 9110 §5.2). A client line followed
// by a proxy-appended line is walked together; taking only the first line
// would let the caller pick the client and claim TLS.
//
// A client can prepend hops to these lists. The trusted peer is the hop that
// delivered the request, so walking from the right, the first element whose
// node is not trusted is the client hop -- including an element with no node,
// `unknown` or an obfuscated name (HAProxy appends a bare `proto=`), since its
// writer did not vouch for anything further left. Such a hop names no client,
// so the client is the nearest trusted address to its right, or the transport
// peer when the hop is the rightmost. Scheme is the proto on the client hop,
// not one from an earlier prepended element -- taking the last proto in the
// whole list would let the caller claim TLS whenever the real hop omitted it.

struct forwarded_client final {
    std::string_view address_;
    std::string_view scheme_;
};

// "[2001:db8::1]:443" -> "2001:db8::1"; "192.0.2.1:8080" -> "192.0.2.1".
// A bare IPv6 literal has colons of its own, so only a bracketed form or a
// single trailing colon can carry a port.
[[nodiscard]] std::string_view forwarded_node_address(std::string_view node_value) noexcept {
    node_value = ::ruvia::http_trim_quoted_field_value(::ruvia::http_trim_ows(node_value));
    if (!node_value.empty() && node_value.front() == '[') {
        const auto close = node_value.find(']');
        return close == std::string_view::npos ? std::string_view{} : node_value.substr(1, close - 1);
    }
    const auto colon = node_value.find(':');
    if (colon != std::string_view::npos && node_value.find(':', colon + 1) == std::string_view::npos) {
        return node_value.substr(0, colon);
    }
    return node_value;
}

[[nodiscard]] std::string_view forwarded_http_scheme_token(std::string_view token) noexcept {
    token = ::ruvia::http_trim_ows(token);
    if (http_ascii_equals_ignore_case(token, "http")) {
        return "http";
    }
    if (http_ascii_equals_ignore_case(token, "https")) {
        return "https";
    }
    return {};
}

template <typename fn_type>
void visit_comma_separated(std::string_view value, fn_type&& fn) {
    // RFC 7239 quoted-string values may contain commas; a quote-blind split
    // would invent hops from inside a single `for="..."` element. Empty list
    // elements are not hops (RFC 9110 §5.6.1).
    http_visit_comma_separated_quoted_field_items(value, [&](std::string_view item) {
        if (!item.empty()) {
            fn(item);
        }
        return true;
    });
}

[[nodiscard]] forwarded_client parse_forwarded_element(std::string_view element) noexcept {
    forwarded_client hop;
    bool saw_for = false;
    bool saw_proto = false;
    // RFC 7239 values are token / quoted-string; a ';' inside quotes is data.
    http_visit_semicolon_parameters_quoted_field(
        element, [&](std::string_view name, std::string_view value) {
            if (http_ascii_equals_ignore_case(name, "for")) {
                if (saw_for) {
                    hop.address_ = {};
                } else {
                    saw_for = true;
                    hop.address_ = forwarded_node_address(value);
                }
            } else if (http_ascii_equals_ignore_case(name, "proto")) {
                if (saw_proto) {
                    hop.scheme_ = {};
                } else {
                    saw_proto = true;
                    hop.scheme_ = forwarded_http_scheme_token(http_trim_quoted_field_value(value));
                }
            }
            return true;
        });
    return hop;
}

// One forwarding list, folded left to right across same-name fields.
class forwarding_chain final {
public:
    void append(const trusted_proxy_set& trusted, forwarded_client hop) noexcept {
        if (size_ == 0) {
            leftmost_ = hop;
        }
        const auto trust = trusted.classify(hop.address_);
        if (trust == trusted_proxy_set::hop_trust::trusted) {
            if (awaiting_nearest_trusted_) {
                nearest_trusted_ = hop.address_;
                awaiting_nearest_trusted_ = false;
            }
        } else {
            client_hop_ = hop;
            client_index_ = size_;
            client_identified_ = trust == trusted_proxy_set::hop_trust::untrusted;
            has_client_hop_ = true;
            nearest_trusted_ = {};
            awaiting_nearest_trusted_ = true;
        }
        ++size_;
    }

    // Every hop trusted: the leftmost is as far as the trusted proxies reach.
    [[nodiscard]] forwarded_client client() const noexcept {
        if (!has_client_hop_) {
            return leftmost_;
        }
        return {client_identified_ ? client_hop_.address_ : nearest_trusted_, client_hop_.scheme_};
    }

    [[nodiscard]] std::size_t client_index() const noexcept {
        return client_index_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    forwarded_client leftmost_;
    forwarded_client client_hop_;
    std::string_view nearest_trusted_;
    std::size_t client_index_{0};
    std::size_t size_{0};
    bool has_client_hop_{false};
    bool client_identified_{false};
    bool awaiting_nearest_trusted_{false};
};

// Resolves what a trusted proxy says about the client. Fields it did not send
// stay empty and the caller keeps what the transport already knows.
[[nodiscard]] forwarded_client resolve_forwarded_client(
    const http_request& request, const trusted_proxy_set& trusted) noexcept {
    const bool has_x_for = request.header("X-Forwarded-For").has_value();
    const bool has_forwarded = !has_x_for && request.header("Forwarded").has_value();
    if (!has_x_for && !has_forwarded) {
        return {};
    }
    const bool check_x_proto =
        has_x_for && trusted.trusts_x_forwarded_proto() && request.header("X-Forwarded-Proto").has_value();
    forwarding_chain forwarded;
    forwarding_chain x_forwarded_for;
    std::string_view single_x_proto_value{};
    std::size_t x_proto_header_count = 0;
    const auto headers = request.headers();
    for (const auto& header : headers) {
        const auto name = header.name();
        if (has_x_for) {
            if (name.size() == 15U && http_ascii_equals_ignore_case(name, "X-Forwarded-For")) {
                visit_comma_separated(header.value(), [&](std::string_view element) {
                    x_forwarded_for.append(trusted, {forwarded_node_address(element), {}});
                });
            } else if (check_x_proto && name.size() == 17U &&
                       http_ascii_equals_ignore_case(name, "X-Forwarded-Proto")) {
                if (x_proto_header_count == 0) {
                    single_x_proto_value = header.value();
                }
                ++x_proto_header_count;
            }
        } else if (name.size() == 9U && http_ascii_equals_ignore_case(name, "Forwarded")) {
            visit_comma_separated(header.value(), [&](std::string_view element) {
                forwarded.append(trusted, parse_forwarded_element(element));
            });
        }
    }
    if (!has_x_for) {
        return forwarded.client();
    }
    // Never combine an XFF address with proto from an unrelated Forwarded
    // chain. XFP is usable only under an explicit sanitizing-proxy contract.
    forwarded_client result_value{x_forwarded_for.client().address_, {}};
    if (x_proto_header_count == 0) {
        return result_value;
    }
    std::size_t proto_count = 0;
    const auto consume_proto_element = [&](std::string_view element) {
        if (proto_count == x_forwarded_for.client_index()) {
            result_value.scheme_ = forwarded_http_scheme_token(element);
        }
        ++proto_count;
    };
    if (x_proto_header_count == 1) {
        visit_comma_separated(single_x_proto_value, consume_proto_element);
    } else {
        for (const auto& header : headers) {
            const auto name = header.name();
            if (name.size() == 17U && http_ascii_equals_ignore_case(name, "X-Forwarded-Proto")) {
                visit_comma_separated(header.value(), consume_proto_element);
            }
        }
    }
    if (x_forwarded_for.size() != proto_count) {
        result_value.scheme_ = {};
    }
    return result_value;
}

}  // namespace detail

conn_info detail::context_services::resolve_conn_info(const http_request& request) const noexcept {
    auto resolved = connection_services_.info_;
    if (!connection_services_.peer_trusted_) {
        return resolved;
    }
    const auto forwarded = detail::resolve_forwarded_client(request, *worker_services_->trusted_proxies_);
    resolved.apply_forwarded(forwarded.address_, forwarded.scheme_);
    return resolved;
}

}  // namespace ruvia
