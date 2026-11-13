#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_base64.h"
#include "ruvia/http/http_client_redirect.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/request_stream.h"
#include "ruvia/web/streaming.h"

#include "client/client_tls.h"
#include "client/http_client_config_storage.h"
#include "client/http_client_pool.h"
#include "client/http_client_registry.h"
#include "client/http_client_request_storage.h"
#include "client/http_client_response_state.h"
#include "http/inline_http_header_views.h"
#include "tls/tls_random.h"

namespace ruvia {
namespace detail {

http_client_websocket_handshake::http_client_websocket_handshake(
    websocket_client_negotiation_config_view config, std::pmr::memory_resource* resource)
    : negotiation_(config, resource),
      selected_subprotocol_(resource) {
    std::array<std::uint8_t, 16> nonce;
    if (!fill_random_bytes(nonce)) {
        throw std::runtime_error("failed to generate WebSocket handshake key");
    }
    http_base64_encode(key_.data(), nonce);
}

http_client_output_queue* http_client_request_storage::output() const noexcept {
    return tunnel_ != nullptr ? &tunnel_->output_ : upload_ != nullptr ? &upload_->output_
                                                                       : nullptr;
}

http_client_request_storage::http_client_request_storage(std::string_view method,
    std::string_view target, std::span<const http_header_view> fields, std::pmr::memory_resource* resource)
    : http_client_request_storage(method, target, resource) {
    if (fields.empty()) {
        return;
    }
    headers_.reserve(fields.size());
    auto* const field_resource = this->resource();
    for (const auto& field : fields) {
        headers_.emplace_back(field.name(), field.value(), field_resource);
    }
}

http_client_request_storage::http_client_request_storage(
    std::string_view method, std::string_view target, std::pmr::memory_resource* resource)
    : method_(method, pmr_resource_or_default(resource)),
      target_(target, method_.get_allocator().resource()),
      headers_(method_.get_allocator().resource()),
      body_(std::string_view{}, method_.get_allocator().resource()),
      tunnel_authority_(method_.get_allocator().resource()),
      tunnel_protocol_(method_.get_allocator().resource()),
      route_authority_(method_.get_allocator().resource()) {}

http_client_request_storage::http_client_request_storage(http_client_request_storage&& other) noexcept
    : options_(std::move(other.options_)),
      informational_(std::move(other.informational_)),
      pool_owner_(std::move(other.pool_owner_)),
      method_(std::move(other.method_)),
      target_(std::move(other.target_)),
      headers_(std::move(other.headers_)),
      body_(std::move(other.body_)),
      tunnel_authority_(std::move(other.tunnel_authority_)),
      tunnel_protocol_(std::move(other.tunnel_protocol_)),
      websocket_(std::move(other.websocket_)),
      host_stop_(std::move(other.host_stop_)),
      is_tunnel_(other.is_tunnel_),
      is_upgrade_(other.is_upgrade_),
      tunnel_(std::exchange(other.tunnel_, nullptr)),
      has_body_(std::exchange(other.has_body_, false)),
      upload_(std::exchange(other.upload_, nullptr)),
      route_authority_(other.route_authority_.get_allocator().resource()) {
    static_assert(std::is_nothrow_move_constructible_v<decltype(method_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(target_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(headers_)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(body_)>);
}

void http_client_request_storage::set_tunnel(std::string_view authority, std::string_view protocol) {
    tunnel_authority_.assign(authority);
    tunnel_protocol_.assign(protocol);
    is_tunnel_ = true;
}

http_client_request_storage& http_client_request_storage::set_body(std::string_view body) {
    body_.assign(body);
    has_body_ = true;
    return *this;
}

namespace {
// host[:port] without userinfo, like the authority of base_url_.
[[nodiscard]] http_authority_view parse_route_authority(std::string_view authority) {
    const auto parsed = parse_http_authority(borrowed_text(authority));
    if (!parsed || authority.find_first_of("@/?#") != std::string_view::npos || parsed->host().empty() ||
        parsed->port_kind() == http_authority_port_kind::empty || parsed->port() == 0) {
        throw std::invalid_argument("HTTP request authority must be host[:port]");
    }
    auto host = parsed->host();
    if (host.starts_with('[')) {
        host = host.substr(1, host.size() - 2);
    }
    validate_client_origin_host(host, "HTTP request authority host must not be empty", "HTTP request authority host is invalid");
    return *parsed;
}
}  // namespace

void validate_http_client_request_options(const http_client_request_options& options) {
    if (options.field_source_ != http_request_field_source::generated &&
        options.field_source_ != http_request_field_source::forwarded) {
        throw std::invalid_argument("invalid HTTP request field source");
    }
    validate_http_client_route_options(options);
}

std::shared_ptr<http_client_informational_callback> share_http_client_informational(
    http_client_informational_callback&& callback) {
    if (!callback) {
        return nullptr;
    }
    return std::allocate_shared<http_client_informational_callback>(
        std::pmr::polymorphic_allocator<http_client_informational_callback>(process_resource()), std::move(callback));
}

void validate_http_client_route_options(const http_client_request_options& options) {
    if (options.authority_) {
        (void)parse_route_authority(*options.authority_);
    }
    if (options.dial_host_) {
        validate_client_origin_host(*options.dial_host_, "HTTP request dial host must not be empty", "HTTP request dial host is invalid");
    }
    if (options.dial_port_ && options.dial_port_->value() == 0) {
        throw std::invalid_argument("HTTP request dial port must be positive");
    }
    if (options.server_name_) {
        validate_client_origin_host(*options.server_name_, "HTTP request TLS server name must not be empty", "HTTP request TLS server name is invalid");
    }
    if (options.versions_) {
        // The alias scheme is checked with the route.
        validate_http_client_versions(*options.versions_, http_scheme::https);
    }
}

http_client_request_options copy_http_client_request_settings(const http_client_request_options& options) {
    // Binding every member keeps this copy exhaustive: a new option fails to compile here.
    const auto& [decode_content, field_source, fresh_connection, timeouts, informational, authority, dial_host, dial_port,
        server_name, versions] = options;
    (void)informational;
    return {.decode_content_ = decode_content,
        .field_source_ = field_source,
        .fresh_connection_ = fresh_connection,
        .timeouts_ = timeouts,
        .informational_ = {},
        .authority_ = authority,
        .dial_host_ = dial_host,
        .dial_port_ = dial_port,
        .server_name_ = server_name,
        .versions_ = versions};
}

void http_client_request_storage::resolve_route(const http_client_config_storage& config) {
    validate_http_client_route_options(options_);
    const bool https = config.scheme_ == http_scheme::https;
    if (options_.server_name_ && !https) {
        throw std::invalid_argument("HTTP request TLS server name requires an https origin");
    }
    if (!config.fixed_origin_ && !options_.authority_) {
        throw std::invalid_argument("HTTP client alias without a fixed origin requires authority_ on each request");
    }
    const port_number default_port = https ? 443 : 80;
    std::optional<http_origin_view> authority_origin;
    std::string_view authority_host;
    if (options_.authority_) {
        const auto parsed = parse_route_authority(*options_.authority_);
        const http_origin_options origin{.host_ = parsed.host(), .port_ = parsed.effective_port(default_port)};
        authority_origin = https ? http_origin_view::https(origin) : http_origin_view::http(origin);
        route_authority_ = make_http_origin_authority(*authority_origin, route_authority_.get_allocator().resource());
        authority_host = parsed.host();
        if (authority_host.starts_with('[')) {
            authority_host = authority_host.substr(1, authority_host.size() - 2);
        }
    }
    const auto origin_value = authority_origin ? *authority_origin : config.origin();
    http_client_route route{.origin_ = origin_value,
        .authority_ = authority_origin ? std::string_view(route_authority_) : std::string_view(config.authority_),
        // An alias without a fixed origin (or dial target) dials the authority.
        .dial_host_ = config.dial_host_.empty() ? authority_host : std::string_view(config.dial_host_),
        .dial_port_ = config.dial_port_ != 0 ? config.dial_port_ : origin_value.port(),
        .server_name_ = {},
        .alias_origin_ = true,
        .alias_transport_ = true,
        .versions_ = options_.versions_.value_or(config.versions_)};
    if (options_.versions_) {
        validate_http_client_versions(*options_.versions_, config.scheme_);
    }
    if (authority_origin) {
        route.alias_origin_ = config.fixed_origin_ && http_authorities_equal(borrowed_text(route.authority_),
                                                          borrowed_text(std::string_view(config.authority_)), default_port);
    }
    if (is_tunnel_ && !is_upgrade_ && options_.authority_) {
        if (websocket_ != nullptr) {
            // The alias authority named by the WebSocket handshake follows the override.
            tunnel_authority_.assign(route.authority_);
        } else if (tunnel_protocol_.empty() || !http_authorities_equal(borrowed_text(std::string_view(tunnel_authority_)), borrowed_text(route.authority_), default_port)) {
            throw std::invalid_argument("a CONNECT request names its own authority");
        }
    }
    if (options_.dial_host_) {
        route.dial_host_ = *options_.dial_host_;
    }
    if (options_.dial_port_) {
        route.dial_port_ = options_.dial_port_->value();
    }
    const auto alias_server_name = https ? config.tls_.tls_host(config.host_) : std::string_view{};
    route.server_name_ = options_.server_name_           ? std::string_view(*options_.server_name_)
                         : !route.alias_origin_ && https ? authority_host
                                                         : alias_server_name;
    route.alias_transport_ = config.fixed_origin_ && http_ascii_equals_ignore_case(route.dial_host_, config.dial_host_) &&
                             route.dial_port_ == config.dial_port_ && http_ascii_equals_ignore_case(route.server_name_, alias_server_name);
    route_.emplace(route);
}

}  // namespace detail

namespace {
void append_query_component(std::pmr::string& output, std::string_view value) {
    for (const unsigned char byte : value) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            output.push_back(static_cast<char>(byte));
        } else {
            http_append_percent_byte(output, byte);
        }
    }
}

void replace_field(std::pmr::vector<http_header>& fields, std::string_view name, std::string_view value) {
    http_header replacement(name, value, fields.get_allocator().resource());
    const auto found = std::ranges::find_if(fields, [name](const http_header& field) {
        return http_ascii_equals_ignore_case(field.name(), name);
    });
    if (found == fields.end()) {
        fields.push_back(std::move(replacement));
    } else {
        *found = std::move(replacement);
    }
}

bool hop_field(std::string_view name) noexcept {
    return http_ascii_equals_ignore_case(name, "connection") || http_ascii_equals_ignore_case(name, "keep-alive") ||
           http_ascii_equals_ignore_case(name, "proxy-connection") || http_ascii_equals_ignore_case(name, "transfer-encoding") ||
           http_ascii_equals_ignore_case(name, "proxy-authenticate") || http_ascii_equals_ignore_case(name, "proxy-authorization") ||
           http_ascii_equals_ignore_case(name, "te") || http_ascii_equals_ignore_case(name, "trailer") ||
           http_ascii_equals_ignore_case(name, "upgrade");
}

bool connection_named_field(std::span<const http_header> fields, std::string_view name) {
    for (const auto& field : fields) {
        if (!http_ascii_equals_ignore_case(field.name(), "connection")) {
            continue;
        }
        auto tokens = field.value();
        while (!tokens.empty()) {
            const auto comma = tokens.find(',');
            auto token = tokens.substr(0, comma);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
                token.remove_prefix(1);
            }
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
                token.remove_suffix(1);
            }
            if (http_ascii_equals_ignore_case(token, name)) {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            tokens.remove_prefix(comma + 1);
        }
    }
    return false;
}

// One bound for the whole forward: commit, body and end share the deadline
// started here and the caller's stop token.
task<void> pipe_response(http_client_response response, detail::http_client_response_state& state,
    response_stream_writer& output, std::pmr::memory_resource* resource, operation_options operation) {
    if (operation.stop_token_.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP client operation cancelled", std::nullopt, &state.connection_info_, state.request_written_);
    }
    const detail::operation_steps steps(std::move(operation));
    stream_commit_result committed{};
    try {
        committed = co_await output.commit(state.downstream_step(steps));
    } catch (...) {
        state.rethrow_downstream_failure(steps);
    }
    if (committed == stream_commit_result::body) {
        co_await state.consume_body<void>(state.downstream_step(steps), &output);
        std::pmr::vector<http_header_view> trailers(pmr_resource_or_default(resource));
        trailers.reserve(response.trailers().size());
        for (const auto& field : response.trailers()) {
            if (!hop_field(field.name()) && !connection_named_field(response.headers(), field.name())) {
                trailers.emplace_back(field.name(), field.value());
            }
        }
        try {
            co_await output.end(trailers, state.downstream_step(steps));
        } catch (...) {
            state.rethrow_downstream_failure(steps);
        }
    }
}

// The origin a request addresses: its authority override, else the alias.
// Redirect targets resolve against it.
[[nodiscard]] http_origin_view request_origin(const http_client_handle& client, const http_client_request_options& options) {
    if (!options.authority_) {
        return client.origin();
    }
    const auto parsed = detail::parse_route_authority(*options.authority_);
    const bool https = client.scheme() == http_scheme::https;
    const http_origin_options origin{.host_ = parsed.host(), .port_ = parsed.effective_port(https ? 443 : 80)};
    return https ? http_origin_view::https(origin) : http_origin_view::http(origin);
}

void clear_route_overrides(http_client_request_options& options) noexcept {
    options.authority_.reset();
    options.dial_host_.reset();
    options.dial_port_.reset();
    options.server_name_.reset();
}

// A task frame may bind exactly one scope. This outer frame binds the
// destination while its child remains bound to the source response.
task<void> pipe_response_in_destination(task<void> source) {
    co_await std::move(source);
}
}  // namespace

http_client_request::http_client_request(const http_client_handle& client, std::string_view method, std::string_view path)
    : client_(client),
      method_(method, process_resource()),
      target_(path, process_resource()),
      headers_(process_resource()),
      body_(process_resource()) {
    client_.registration_.require_active();
    if (!is_valid_http_method_token(method) ||
        (!is_valid_http_origin_form_target(path) && !(method == "OPTIONS" && path == "*"))) {
        throw std::invalid_argument("HTTP request requires a valid method and relative origin-form target");
    }
}

http_client_request http_client_handle::request(std::string_view method, std::string_view path) const {
    registration_.require_active();
    return http_client_request(*this, method, path);
}
http_client_request http_client_handle::get(std::string_view path) const {
    return request("GET", path);
}
http_client_request http_client_handle::post(std::string_view path) const {
    return request("POST", path);
}
http_client_request http_client_handle::put(std::string_view path) const {
    return request("PUT", path);
}
http_client_request http_client_handle::patch(std::string_view path) const {
    return request("PATCH", path);
}
http_client_request http_client_handle::del(std::string_view path) const {
    return request("DELETE", path);
}
http_client_request http_client_handle::head(std::string_view path) const {
    return request("HEAD", path);
}
http_client_request http_client_handle::options(std::string_view path) const {
    return request("OPTIONS", path);
}

http_client_request& http_client_request::header(std::string_view name, std::string_view value) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    replace_field(headers_, name, value);
    return *this;
}
http_client_request& http_client_request::headers(std::span<const http_header_view> fields) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    const auto old_size = headers_.size();
    headers_.reserve(old_size + fields.size());
    try {
        for (const auto& field : fields) {
            headers_.emplace_back(field.name(), field.value(), headers_.get_allocator().resource());
        }
    } catch (...) {
        while (headers_.size() > old_size) {
            headers_.pop_back();
        }
        throw;
    }
    return *this;
}
http_client_request& http_client_request::query(std::string_view name, std::string_view value) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    if (target_ == "*") {
        throw std::invalid_argument("asterisk target cannot contain query parameters");
    }
    const auto old_size = target_.size();
    const char separator = target_.find('?') == std::string_view::npos ? '?' : '&';
    try {
        target_.push_back(separator);
        append_query_component(target_, name);
        target_.push_back('=');
        append_query_component(target_, value);
    } catch (...) {
        target_.resize(old_size);
        throw;
    }
    return *this;
}
http_client_request& http_client_request::body(std::string_view value) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    std::pmr::string owned(value, body_.get_allocator().resource());
    body_.swap(owned);
    has_body_ = true;
    return *this;
}
http_client_request& http_client_request::body(std::span<const std::byte> value) {
    return body(std::string_view(reinterpret_cast<const char*>(value.data()), value.size()));
}
http_client_request& http_client_request::configure(http_client_request_options options) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    detail::validate_http_client_request_options(options);
    informational_callback_ = detail::share_http_client_informational(std::move(options.informational_));
    request_options_ = std::move(options);
    return *this;
}
http_client_request& http_client_request::redirect_policy(
    move_only_function<task<bool>(http_origin_view, stop_token)> policy) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    redirect_policy_ = std::move(policy);
    return *this;
}

task<http_client_tunnel_result> http_client_request::upgrade(http_client_tunnel_config config) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    client_.registration_.require_active();
    if (config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes) {
        throw std::invalid_argument("invalid HTTP Upgrade tunnel policy");
    }
    const detail::inline_http_header_views<16> fields(headers_, headers_.get_allocator().resource());
    detail::http_client_request_storage request(method_, target_, fields.view(), client_.resource_);
    request.options_ = detail::copy_http_client_request_settings(request_options_);
    request.informational_ = informational_callback_;
    request.pool_owner_ = client_.pool_owner_;
    request.set_host_stop(client_.host_stop_token_);
    request.set_upgrade();
    if (has_body_) {
        request.set_body(body_);
    }
    auto operation = make_scoped_operation(client_.registration_.scope(),
        client_.pool_->open_tunnel(std::move(request), config, client_.options_));
    consumed_ = true;
    std::pmr::string(method_.get_allocator()).swap(method_);
    std::pmr::string(target_.get_allocator()).swap(target_);
    std::pmr::vector<http_header>(headers_.get_allocator()).swap(headers_);
    std::pmr::string(body_.get_allocator()).swap(body_);
    return operation;
}

std::pmr::vector<http_header_view> http_client_request::field_views() const {
    std::pmr::vector<http_header_view> fields(headers_.get_allocator().resource());
    fields.reserve(headers_.size());
    for (const auto& field : headers_) {
        fields.emplace_back(field.name(), field.value());
    }
    return fields;
}

task<http_client_response> http_client_request::send() {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    client_.registration_.require_active();
    consumed_ = true;
    return make_scoped_operation(client_.registration_.scope(), send_owned(std::move(*this)));
}

task<http_client_response> http_client_request::send_owned(http_client_request request) {
    // One deadline spans every redirect hop; each hop receives it as an absolute bound.
    const operation_deadline total(request.client_.options_.timeout_.has_value()
                                       ? request.client_.options_.timeout_
                                       : std::optional<operation_timeout>(request.client_.pool_->config().request_timeout_));
    for (std::size_t redirects = 0;; ++redirects) {
        request.client_.registration_.require_active();
        if (total.expired()) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP redirect deadline expired", std::nullopt, nullptr, redirects != 0);
        }
        request.client_.options_.timeout_ = total.timeout();
        std::optional<task<http_client_response>> dispatched;
        {
            const detail::inline_http_header_views<16> fields(
                request.headers_, request.headers_.get_allocator().resource());
            const http_client_request_view view{
                .method_ = request.method_,
                .target_ = request.target_,
                .headers_ = fields.view(),
                .content_ = request.has_body_ ? http_client_request_content_view::bytes(request.body_)
                                              : http_client_request_content_view::none()};
            dispatched.emplace(request.client_.dispatch(view, request.request_options_, request.informational_callback_));
        }
        if (redirects == request.max_redirects_) {
            std::pmr::string(request.method_.get_allocator()).swap(request.method_);
            std::pmr::string(request.target_.get_allocator()).swap(request.target_);
            std::pmr::vector<http_header>(request.headers_.get_allocator()).swap(request.headers_);
            std::pmr::string(request.body_.get_allocator()).swap(request.body_);
            co_return co_await std::move(*dispatched);
        }
        auto response = co_await std::move(*dispatched);
        const auto status = response.status().value();
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
            co_return response;
        }
        std::optional<std::string_view> location;
        for (const auto& field : response.headers()) {
            if (http_ascii_equals_ignore_case(field.name(), "location")) {
                if (location) {
                    throw http_client_error(make_error_code(http_client_errc::protocol_error), "repeated redirect Location");
                }
                location = field.value();
            }
        }
        if (!location) {
            co_return response;
        }
        auto resolved = resolve_http_client_redirect_target(request_origin(request.client_, request.request_options_),
            {.current_target_ = request.target_, .location_ = *location, .resource_ = request.client_.resource_});
        auto* next = std::get_if<http_client_resolved_redirect>(&resolved);
        if (next == nullptr) {
            co_return response;
        }
        auto next_pool = request.client_.pool_owner_;
        if (next->cross_origin()) {
            if (request.redirect_policy_) {
                stop_source deadline_stop;
                stop_group cancellation{request.client_.options_.stop_token_, request.client_.host_stop_token_,
                    deadline_stop.token()};
                worker_timer_registration timer;
                if (const auto deadline_value = total.deadline()) {
                    (void)request.client_.pool_->worker().schedule_timer(timer, *deadline_value,
                        [&deadline_stop](worker_timer_outcome outcome) noexcept {
                            if (outcome == worker_timer_outcome::expired) {
                                deadline_stop.request_stop();
                            }
                        });
                }
                const auto require_live_policy = [&]() {
                    if (total.expired()) {
                        throw http_client_error(std::make_error_code(std::errc::timed_out),
                            "HTTP redirect policy deadline expired", std::nullopt, &response.connection_info());
                    }
                    if (cancellation.token().stop_requested()) {
                        throw http_client_error(std::make_error_code(std::errc::operation_canceled),
                            "HTTP redirect policy cancelled", std::nullopt, &response.connection_info());
                    }
                };
                require_live_policy();
                const auto accepted = co_await request.redirect_policy_(next->origin(), cancellation.token());
                require_live_policy();
                if (!accepted) {
                    co_return response;
                }
                request.client_.registration_.require_active();
            } else if (request.client_.scheme() == http_scheme::https && next->scheme() != http_scheme::https) {
                co_return response;
            }
            auto authority = make_http_origin_authority(next->origin(), request.client_.resource_);
            // Route overrides belong to the origin that set them.
            clear_route_overrides(request.request_options_);
            if (!request.client_.pool_->config().fixed_origin_ && request.redirect_policy_ &&
                next->scheme() == request.client_.scheme()) {
                // An alias without a fixed origin reaches the accepted origin itself.
                request.request_options_.authority_.emplace(std::string_view(authority));
            } else {
                if (request.client_.registry_ == nullptr) {
                    co_return response;
                }
                next_pool = request.client_.registry_->find_origin(next->scheme(), authority);
                if (next_pool == nullptr) {
                    co_return response;
                }
            }
        }
        const http_client_request_view view{
            .method_ = request.method_,
            .target_ = request.target_,
            .content_ = request.has_body_ ? http_client_request_content_view::bytes(request.body_)
                                          : http_client_request_content_view::none()};
        const auto plan = plan_http_client_redirect_request(view, {.status_ = response.status(), .resource_ = request.client_.resource_});
        request.method_.assign(plan.method());
        request.target_.assign(next->target());
        const bool drop = plan.content_disposition() == http_client_redirect_content_disposition::drop;
        std::erase_if(request.headers_, [&](const http_header& field) {
            const auto name = field.name();
            return http_ascii_equals_ignore_case(name, "host") ||
                   (next->cross_origin() && (http_ascii_equals_ignore_case(name, "authorization") ||
                                                http_ascii_equals_ignore_case(name, "proxy-authorization") || http_ascii_equals_ignore_case(name, "cookie"))) ||
                   (drop && (http_ascii_equals_ignore_case(name.substr(0, 8), "content-") ||
                                http_ascii_equals_ignore_case(name, "transfer-encoding") || http_ascii_equals_ignore_case(name, "trailer") ||
                                http_ascii_equals_ignore_case(name, "expect")));
        });
        if (drop) {
            request.has_body_ = false;
            std::pmr::string(request.body_.get_allocator()).swap(request.body_);
        }
        request.client_.pool_owner_ = std::move(next_pool);
        request.client_.pool_ = request.client_.pool_owner_.get();
    }
}

task<http_client_exchange> http_client_request::upload(http_client_upload_config config) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    if (has_body_) {
        throw std::invalid_argument("streamed upload cannot also have a buffered body");
    }
    const detail::inline_http_header_views<16> fields(headers_, headers_.get_allocator().resource());
    auto dispatched = client_.dispatch_upload({.method_ = method_, .target_ = target_, .headers_ = fields.view()}, config, request_options_, informational_callback_);
    consumed_ = true;
    std::pmr::string(method_.get_allocator()).swap(method_);
    std::pmr::string(target_.get_allocator()).swap(target_);
    std::pmr::vector<http_header>(headers_.get_allocator()).swap(headers_);
    std::pmr::string(body_.get_allocator()).swap(body_);
    return dispatched;
}

http_client_connect_request::http_client_connect_request(const http_client_handle& client,
    std::string_view authority, std::string_view target, bool udp)
    : client_(client),
      authority_(authority, process_resource()),
      protocol_(process_resource()),
      target_(target, process_resource()),
      headers_(process_resource()),
      udp_(udp) {}
http_client_connect_request http_client_handle::connect(std::string_view authority) const {
    registration_.require_active();
    return http_client_connect_request(*this, authority, {}, false);
}
http_client_connect_request http_client_handle::udp(std::string_view path) const {
    registration_.require_active();
    return http_client_connect_request(*this, pool_->config().authority_, path, true);
}
http_client_connect_request& http_client_connect_request::header(std::string_view name, std::string_view value) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    replace_field(headers_, name, value);
    return *this;
}
http_client_connect_request& http_client_connect_request::headers(std::span<const http_header_view> fields) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    const auto old_size = headers_.size();
    headers_.reserve(old_size + fields.size());
    try {
        for (const auto& field : fields) {
            headers_.emplace_back(field.name(), field.value(), headers_.get_allocator().resource());
        }
    } catch (...) {
        while (headers_.size() > old_size) {
            headers_.pop_back();
        }
        throw;
    }
    return *this;
}
http_client_connect_request& http_client_connect_request::configure(http_client_request_options options) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    detail::validate_http_client_request_options(options);
    informational_callback_ = detail::share_http_client_informational(std::move(options.informational_));
    request_options_ = std::move(options);
    return *this;
}
http_client_connect_request& http_client_connect_request::protocol(std::string_view value) {
    if (consumed_ || udp_) {
        throw std::logic_error("CONNECT protocol cannot be changed");
    }
    protocol_.assign(value);
    return *this;
}
http_client_connect_request& http_client_connect_request::target(std::string_view value) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    target_.assign(value);
    return *this;
}
task<http_client_tunnel_result> http_client_connect_request::open(http_client_tunnel_config config) {
    if (consumed_) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    const detail::inline_http_header_views<16> fields(headers_, headers_.get_allocator().resource());
    auto dispatched = udp_
                          ? client_.dispatch_udp({.target_ = target_, .headers_ = fields.view()}, config, request_options_, informational_callback_)
                          : client_.dispatch_connect(
                                {.authority_ = authority_, .protocol_ = protocol_, .target_ = target_, .headers_ = fields.view()}, config, request_options_, informational_callback_);
    consumed_ = true;
    std::pmr::string(authority_.get_allocator()).swap(authority_);
    std::pmr::string(protocol_.get_allocator()).swap(protocol_);
    std::pmr::string(target_.get_allocator()).swap(target_);
    std::pmr::vector<http_header>(headers_.get_allocator()).swap(headers_);
    return dispatched;
}

task<void> http_client_response::pipe_to(context& output, operation_options operation) & {
    return pipe_to_destination(output.draft(), output.stream(), output.operation_scope_, std::move(operation));
}

task<void> http_client_response::pipe_to(request_stream& output, operation_options operation) & {
    return pipe_to_destination(output.draft(), output.response(), output.operation_scope_, std::move(operation));
}

task<void> http_client_response::pipe_to_destination(context_response_editor draft, response_stream_writer& writer,
    ::ruvia::operation_scope& destination, operation_options operation) {
    if (state_ == nullptr) {
        throw std::logic_error("response owner has moved");
    }
    state_->require_owner_worker("HTTP response requires its owner worker");
    if (!state_->body_operation_scope_.active() || !destination.active()) {
        throw std::logic_error("HTTP response pipe_to scope is closed");
    }
    draft.status(status());
    const auto fields = headers();
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        if (!hop_field(field->name()) && !connection_named_field(fields, field->name()) &&
            !http_ascii_equals_ignore_case(field->name(), "content-length")) {
            const bool repeated = std::any_of(fields.begin(), field, [&](const http_header& previous) {
                return http_ascii_equals_ignore_case(previous.name(), field->name());
            });
            draft.header(field->name(), field->value(),
                {.mode_ = repeated ? http_response_header_mode::append : http_response_header_mode::replace});
        }
    }
    auto source = make_scoped_operation(state_->body_operation_scope_,
        pipe_response(http_client_response(state_), *state_, writer, state_->resource_, std::move(operation)));
    return make_scoped_operation(destination, pipe_response_in_destination(std::move(source)));
}

#ifdef RUVIA_ENABLE_HTTP3
namespace {
task<void> migrate_client_pool(std::shared_ptr<detail::http_client_pool> pool,
    std::pmr::string address, std::uint16_t port, operation_options options, stop_token host_stop) {
    co_await pool->migrate_http3(std::move(address), port, std::move(options), std::move(host_stop));
}
}  // namespace
#endif

task<void> http_client_handle::migrate(std::string_view local_address, std::uint16_t port) const {
    registration_.require_active();
#ifdef RUVIA_ENABLE_HTTP3
    return make_scoped_operation(registration_.scope(),
        migrate_client_pool(pool_owner_, std::pmr::string(local_address, process_resource()), port, options_, host_stop_token_));
#else
    (void)local_address;
    (void)port;
    throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP/3 support is disabled");
#endif
}
}  // namespace ruvia
