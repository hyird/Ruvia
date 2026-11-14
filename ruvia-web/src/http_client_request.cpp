#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/error.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>

#include "borrowed_text.h"
#include "client_host_resolver.h"
#include "context.h"
#include "context_access.h"
#include "event_loop.h"
#include "http1_client_request.h"
#include "http_authority.h"
#include "http_client.h"
#include "http_client_config_storage.h"
#include "http_client_pool.h"
#include "http_client_redirect.h"
#include "http_client_registry.h"
#include "http_client_request_storage.h"
#include "http_client_request_view.h"
#include "http_client_response_state.h"
#include "http_client_types.h"
#include "http_connect_udp.h"
#include "http_cookie.h"
#include "http_header.h"
#include "http_message.h"
#include "http_protocol.h"
#include "http_request.h"
#include "inline_storage.h"
#include "ip_address.h"
#include "operation_options.h"
#include "operation_scope.h"
#include "stop_token.h"
#include "task.h"
#include "url_encoding.h"
#include "worker_ref.h"
#include "worker_timer.h"

namespace ruvia {
namespace detail {

http_client_output_queue* http_client_request_storage::output() const noexcept {
    return tunnel_ != nullptr ? &tunnel_->output_ : (upload_ != nullptr ? &upload_->output_ : nullptr);
}

http_client_request_storage::http_client_request_storage(std::string_view method, std::string_view target, std::span<const http_header_view> fields) :
    http_client_request_storage(method, target) {
    if (fields.empty()) {
        return;
    }
    headers_.reserve(fields.size());
    for (const auto& field : fields) {
        headers_.emplace_back(field.name(), field.value());
    }
}

http_client_request_storage::http_client_request_storage(std::string_view method, std::string_view target) :
    method_(method),
    target_(target),
    route_authority_(method_) {}

http_client_request_storage::http_client_request_storage(http_client_request_storage&& other) noexcept :
    options_(std::move(other.options_)),
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
    route_authority_(other.route_authority_) {
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
    if (!parsed || authority.find_first_of("@/?#") != std::string_view::npos || parsed->host().empty() || parsed->port_kind() == http_authority_port_kind::empty || parsed->port() == 0) {
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
    if (options.field_source_ != http_request_field_source::generated && options.field_source_ != http_request_field_source::forwarded) {
        throw std::invalid_argument("invalid HTTP request field source");
    }
    validate_http_client_route_options(options);
}

std::shared_ptr<http_client_informational_callback> share_http_client_informational(http_client_informational_callback&& callback) {
    if (!callback) {
        return nullptr;
    }
    return std::make_shared<http_client_informational_callback>(std::move(callback));
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
    const auto& [decode_content, field_source, fresh_connection, timeouts, informational, authority, dial_host, dial_port, server_name, versions] = options;
    (void)informational;
    return {.decode_content_ = decode_content, .field_source_ = field_source, .fresh_connection_ = fresh_connection, .timeouts_ = timeouts, .informational_ = {}, .authority_ = authority, .dial_host_ = dial_host, .dial_port_ = dial_port, .server_name_ = server_name, .versions_ = versions};
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
        route_authority_ = make_http_origin_authority(*authority_origin);
        authority_host = parsed.host();
        if (authority_host.starts_with('[')) {
            authority_host = authority_host.substr(1, authority_host.size() - 2);
        }
    }
    const auto origin_value = authority_origin ? *authority_origin : config.origin();
    http_client_route route{
        .origin_ = origin_value,
        .authority_ = authority_origin ? std::string_view(route_authority_) : std::string_view(config.authority_),
        // An alias without a fixed origin (or dial target) dials the authority.
        .dial_host_ = config.dial_host_.empty() ? authority_host : std::string_view(config.dial_host_),
        .dial_port_ = config.dial_port_ != 0 ? config.dial_port_ : origin_value.port(),
        .server_name_ = {},
        .alias_origin_ = true,
        .alias_transport_ = true,
        .versions_ = options_.versions_.value_or(config.versions_)
    };
    if (options_.versions_) {
        validate_http_client_versions(*options_.versions_, config.scheme_);
    }
    if (authority_origin) {
        route.alias_origin_ = config.fixed_origin_ && http_authorities_equal(borrowed_text(route.authority_), borrowed_text(std::string_view(config.authority_)), default_port);
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
    route.server_name_ = options_.server_name_ ? std::string_view(*options_.server_name_) : (!route.alias_origin_ && https ? authority_host : alias_server_name);
    route.alias_transport_ = config.fixed_origin_ && http_ascii_equals_ignore_case(route.dial_host_, config.dial_host_) && route.dial_port_ == config.dial_port_ && http_ascii_equals_ignore_case(route.server_name_, alias_server_name);
    route_.emplace(route);
}

client_request_policy::client_request_policy(const http_client_config_storage& config) :
    config_(config),
    jar_(config.scheme_, config.host_, {.max_cookies_ = config.max_cookies_, .max_cookie_bytes_ = config.max_cookie_bytes_}) {
    for (const auto& cookie : config.cookies_) {
        jar_.add_configured_cookie(cookie.name(), cookie.value());
    }
}

bool client_request_policy::addresses_origin(const http_client_request_storage& request) const noexcept {
    return (!request.is_tunnel() || request.is_upgrade()) && request.route().alias_origin_;
}

http_client_request_view client_request_policy::prepare_request(const http_client_request_storage& request, client_prepared_request_scratch& scratch) {
    const auto& input_headers = request.headers();
    const auto max_prepared_headers = input_headers.size() + 2;
    http_header_view* prepared_begin = scratch.inline_headers_.data();
    if (max_prepared_headers > scratch.inline_headers_.size()) {
        scratch.overflow_headers_.resize(max_prepared_headers);
        prepared_begin = scratch.overflow_headers_.data();
    }
    std::size_t index = 0;
    bool add_user_agent = !config_.user_agent_.empty();
    scratch.overflow_cookie_header_.clear();
    std::size_t inline_cookie_size = 0;
    bool cookie_overflowed = false;
    // RFC 6265 section 4.2.1: cookie-pairs are joined by "; ".
    const auto ensure_cookie_space = [&](std::size_t count) -> char* {
        const auto current_size = cookie_overflowed ? scratch.overflow_cookie_header_.size() : inline_cookie_size;
        const auto separator_bytes = current_size == 0 ? std::size_t{0} : std::size_t{2};
        const auto total_needed = separator_bytes + count;
        if (total_needed == 0) {
            return scratch.inline_cookie_header_.data();
        }
        if (!cookie_overflowed && total_needed <= scratch.inline_cookie_header_.size() - inline_cookie_size) {
            char* cursor = scratch.inline_cookie_header_.data() + inline_cookie_size;
            if (separator_bytes != 0) {
                *cursor++ = ';';
                *cursor++ = ' ';
            }
            inline_cookie_size += total_needed;
            return cursor;
        }
        if (!cookie_overflowed) {
            scratch.overflow_cookie_header_.reserve(std::max(inline_cookie_size + total_needed, scratch.inline_cookie_header_.size() * 2));
            scratch.overflow_cookie_header_.append(scratch.inline_cookie_header_.data(), inline_cookie_size);
            cookie_overflowed = true;
        }
        const auto old_size = scratch.overflow_cookie_header_.size();
        if (old_size + total_needed > scratch.overflow_cookie_header_.capacity()) {
            scratch.overflow_cookie_header_.reserve(std::max(old_size + total_needed, scratch.overflow_cookie_header_.capacity() * 2));
        }
        scratch.overflow_cookie_header_.resize(old_size + total_needed);
        char* cursor = scratch.overflow_cookie_header_.data() + old_size;
        if (separator_bytes != 0) {
            *cursor++ = ';';
            *cursor++ = ' ';
        }
        return cursor;
    };
    // Request storage preserves caller field-name spelling.
    for (const auto& field : input_headers) {
        const auto name = field.name();
        if (add_user_agent && name.size() == 10U && http_ascii_equals_ignore_case(name, "user-agent")) {
            add_user_agent = false;
        }
        if (name.size() == 6U && http_ascii_equals_ignore_case(name, "cookie")) {
            const auto value = field.value();
            std::ranges::copy(value, ensure_cookie_space(value.size()));
            continue;
        }
        prepared_begin[index++] = http_header_view{name, field.value()};
    }

    // RFC 9110 §9.3.8: TRACE reflects the request, so stored credentials stay
    // out; caller-written Cookie fields above remain the caller's choice.
    // Stored cookies belong to the fixed origin (RFC 6265 §5.4): a tunnel to
    // another authority carries none. Empty jars have no expiry or path
    // constraints to inspect.
    if (!jar_.empty() && request.method() != "TRACE" && addresses_origin(request)) {
        const bool include_received = config_.received_cookies_ != http_client_received_cookie_policy::ignore;
        jar_.visit_matching_cookies(request.target(), include_received, std::chrono::system_clock::now(), [&](std::string_view name, std::string_view value) {
            const auto pair_size = cookie_request_pair_size(name, value);
            (void)write_cookie_request_pair(ensure_cookie_space(pair_size), name, value);
        });
    }
    const auto cookie_header_view = cookie_overflowed ? std::string_view(scratch.overflow_cookie_header_) : std::string_view(scratch.inline_cookie_header_.data(), inline_cookie_size);
    if (add_user_agent) {
        prepared_begin[index++] = http_header_view{"user-agent", config_.user_agent_};
    }
    if (!cookie_header_view.empty()) {
        prepared_begin[index++] = http_header_view{"cookie", cookie_header_view};
    }
    http_client_request_view result;
    result.method_ = request.method();
    result.target_ = request.target();
    result.headers_ = std::span<const http_header_view>(prepared_begin, index);
    result.content_ = request.has_body() ? http_client_request_content_view::bytes(request.body()) : http_client_request_content_view::none();
    return result;
}

void client_request_policy::retain_response_cookies(std::string_view request_target, std::span<const http_header> headers) {
    if (config_.received_cookies_ == http_client_received_cookie_policy::ignore) {
        return;
    }
    jar_.retain_response_cookies(request_target, headers);
}

}  // namespace detail

namespace {
void append_query_component(std::string& output, std::string_view value) {
    for (const unsigned char byte : value) {
        if (is_uri_unreserved_byte(byte)) {
            output.push_back(static_cast<char>(byte));
        } else {
            http_append_percent_byte(output, byte);
        }
    }
}

void replace_field(std::vector<http_header>& fields, std::string_view name, std::string_view value) {
    http_header replacement(name, value);
    const auto found = std::ranges::find_if(fields, [name](const http_header& field) {
        return http_ascii_equals_ignore_case(field.name(), name);
    });
    if (found == fields.end()) {
        fields.push_back(std::move(replacement));
    } else {
        *found = std::move(replacement);
    }
}

// The origin a request addresses: its authority override, else the alias.
// Redirect targets resolve against it.
[[nodiscard]] http_origin_view request_origin(const http_client_ref& client, const http_client_request_options& options) {
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

}  // namespace

http_client_request::http_client_request(const http_client_ref& client, std::string_view method, std::string_view path) :
    client_(client),
    method_(method),
    target_(path),
    headers_(),
    body_() {
    client_.registration_.require_active();
    if (!is_valid_http_method_token(method) || (!is_valid_http_origin_form_target(path) && !(method == "OPTIONS" && path == "*"))) {
        throw std::invalid_argument("HTTP request requires a valid method and relative origin-form target");
    }
}

http_client_request http_client_ref::request(std::string_view method, std::string_view path) const {
    registration_.require_active();
    return http_client_request(*this, method, path);
}
http_client_request http_client_ref::get(std::string_view path) const {
    return request("GET", path);
}
http_client_request http_client_ref::post(std::string_view path) const {
    return request("POST", path);
}
http_client_request http_client_ref::put(std::string_view path) const {
    return request("PUT", path);
}
http_client_request http_client_ref::patch(std::string_view path) const {
    return request("PATCH", path);
}
http_client_request http_client_ref::del(std::string_view path) const {
    return request("DELETE", path);
}
http_client_request http_client_ref::head(std::string_view path) const {
    return request("HEAD", path);
}
http_client_request http_client_ref::options(std::string_view path) const {
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
            headers_.emplace_back(field.name(), field.value());
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
    std::string owned(value);
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
http_client_request& http_client_request::on_redirect(http_client_redirect_observer observer) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    redirect_observer_ = std::move(observer);
    return *this;
}
http_client_request& http_client_request::on_redirect(http_client_async_redirect_observer observer) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    async_redirect_observer_ = std::move(observer);
    return *this;
}
http_client_request& http_client_request::redirects(std::size_t maximum) {
    if (consumed_) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    max_redirects_ = maximum;
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
    const detail::inline_http_header_views<16> fields(headers_);
    detail::http_client_request_storage request(method_, target_, fields.view());
    request.options_ = detail::copy_http_client_request_settings(request_options_);
    request.informational_ = informational_callback_;
    request.pool_owner_ = client_.pool_owner_;
    request.set_host_stop(client_.host_stop_token_);
    request.set_upgrade();
    if (has_body_) {
        request.set_body(body_);
    }
    auto operation = make_scoped_operation(client_.registration_.scope(), client_.pool_->open_tunnel(std::move(request), config, client_.options_));
    consumed_ = true;
    std::string(method_.get_allocator()).swap(method_);
    std::string(target_.get_allocator()).swap(target_);
    std::vector<http_header>(headers_.get_allocator()).swap(headers_);
    std::string(body_.get_allocator()).swap(body_);
    return operation;
}

std::vector<http_header_view> http_client_request::field_views() const {
    std::vector<http_header_view> fields;
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

namespace {

[[nodiscard]] std::string format_origin_target_url(const http_origin_view& origin, std::string_view target) {
    const auto authority = make_http_origin_authority(origin);
    const std::string_view scheme_prefix = origin.scheme() == http_scheme::https ? "https://" : "http://";
    std::string url;
    url.reserve(scheme_prefix.size() + authority.size() + target.size());
    url.append(scheme_prefix);
    url.append(authority);
    if (!target.empty() && target.front() != '/' && target != "*") {
        url.push_back('/');
    }
    url.append(target);
    return url;
}

}  // namespace

task<http_client_response> http_client_request::send_owned(http_client_request request) {
    // One deadline spans every redirect hop; each hop receives it as an absolute bound.
    const operation_deadline total(request.client_.options_.timeout_.has_value() ? request.client_.options_.timeout_ : std::optional<operation_timeout>(request.client_.pool_->config().request_timeout_));
    co_return co_await follow_redirects(std::move(request), std::nullopt, total, false);
}

task<http_client_response> http_client_request::follow_redirects(http_client_request request, std::optional<http_client_response> initial_response, const operation_deadline& total, bool upload_initial_hop) {
    for (std::size_t redirects = 0;; ++redirects) {
        std::optional<http_client_response> hop_response;
        if (redirects == 0 && initial_response.has_value()) {
            hop_response.emplace(std::move(*initial_response));
            initial_response.reset();
        } else {
            request.client_.registration_.require_active();
            if (total.expired()) {
                throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP redirect deadline expired", std::nullopt, nullptr, redirects != 0, redirects);
            }
            request.client_.options_.timeout_ = total.timeout();
            std::optional<task<http_client_response>> dispatched;
            {
                const detail::inline_http_header_views<16> fields(request.headers_);
                const http_client_request_view view{.method_ = request.method_, .target_ = request.target_, .headers_ = fields.view(), .content_ = request.has_body_ ? http_client_request_content_view::bytes(request.body_) : http_client_request_content_view::none()};
                dispatched.emplace(request.client_.dispatch(view, request.request_options_, request.informational_callback_));
            }
            if (redirects == request.max_redirects_) {
                std::string(request.method_.get_allocator()).swap(request.method_);
                std::string(request.target_.get_allocator()).swap(request.target_);
                std::vector<http_header>(request.headers_.get_allocator()).swap(request.headers_);
                std::string(request.body_.get_allocator()).swap(request.body_);
                try {
                    co_return co_await std::move(*dispatched);
                } catch (const http_client_error& error) {
                    if (redirects != 0 && error.redirect_hops() == 0) {
                        throw http_client_error(error.code(), error.what(), error.status(), error.connection_info(), error.request_written(), redirects);
                    }
                    throw;
                }
            }
            try {
                hop_response.emplace(co_await std::move(*dispatched));
            } catch (const http_client_error& error) {
                if (redirects != 0 && error.redirect_hops() == 0) {
                    throw http_client_error(error.code(), error.what(), error.status(), error.connection_info(), error.request_written(), redirects);
                }
                throw;
            }
        }

        auto response = std::move(*hop_response);
        if (redirects == request.max_redirects_) {
            co_return response;
        }
        const auto status = response.status().value();
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
            co_return response;
        }
        const http_client_request_view current_view{.method_ = request.method_, .target_ = request.target_, .content_ = request.has_body_ ? http_client_request_content_view::bytes(request.body_) : http_client_request_content_view::none()};
        const auto plan = plan_http_client_redirect_request(current_view, {.status_ = response.status()});
        if (upload_initial_hop && redirects == 0 && plan.content_disposition() != http_client_redirect_content_disposition::drop) {
            co_return response;
        }

        std::optional<std::string_view> location;
        for (const auto& field : response.headers()) {
            if (http_ascii_equals_ignore_case(field.name(), "location")) {
                if (location) {
                    throw http_client_error(make_error_code(http_client_errc::protocol_error), "repeated redirect Location", std::nullopt, &response.connection_info(), true, redirects);
                }
                location = field.value();
            }
        }
        if (!location) {
            co_return response;
        }
        const auto current_origin = request_origin(request.client_, request.request_options_);
        auto resolved = resolve_http_client_redirect_target(current_origin, {.current_target_ = request.target_, .location_ = *location});
        auto* next = std::get_if<http_client_resolved_redirect>(&resolved);
        if (next == nullptr) {
            co_return response;
        }

        if (request.redirect_observer_ || request.async_redirect_observer_) {
            const auto source_url = format_origin_target_url(current_origin, request.target_);
            const auto target_url = format_origin_target_url(next->origin(), next->target());
            const http_client_redirect_hop hop_event(redirects, current_origin, request.target_, source_url, response.connection_info(), response.status(), *location, next->origin(), next->target(), target_url, next->cross_origin());
            if (request.redirect_observer_) {
                if (!request.redirect_observer_(hop_event)) {
                    co_return response;
                }
                request.client_.registration_.require_active();
            }
            if (request.async_redirect_observer_) {
                stop_source deadline_stop;
                stop_group cancellation{request.client_.options_.stop_token_, request.client_.host_stop_token_, deadline_stop.token()};
                worker_timer_registration timer;
                if (const auto deadline_value = total.deadline()) {
                    (void)request.client_.pool_->worker().schedule_timer(timer, *deadline_value, [&deadline_stop](worker_timer_outcome outcome) noexcept {
                        if (outcome == worker_timer_outcome::expired) {
                            deadline_stop.request_stop();
                        }
                    });
                }
                const auto require_live_observer = [&]() {
                    if (total.expired()) {
                        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP redirect policy deadline expired", std::nullopt, &response.connection_info(), true, redirects);
                    }
                    if (cancellation.token().stop_requested()) {
                        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP redirect policy cancelled", std::nullopt, &response.connection_info(), true, redirects);
                    }
                };
                require_live_observer();
                const auto accepted = co_await request.async_redirect_observer_(hop_event, cancellation.token());
                require_live_observer();
                if (!accepted) {
                    co_return response;
                }
                request.client_.registration_.require_active();
            }
        }

        const bool has_custom_policy = static_cast<bool>(request.redirect_observer_) || static_cast<bool>(request.async_redirect_observer_);
        auto next_pool = request.client_.pool_owner_;
        if (next->cross_origin()) {
            if (!has_custom_policy && request.client_.scheme() == http_scheme::https && next->scheme() != http_scheme::https) {
                co_return response;
            }
            auto authority = make_http_origin_authority(next->origin());
            // Route overrides belong to the origin that set them.
            clear_route_overrides(request.request_options_);
            if (!request.client_.pool_->config().fixed_origin_ && has_custom_policy && next->scheme() == request.client_.scheme()) {
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

        if (upload_initial_hop && redirects == 0 && response.state_ != nullptr && response.state_->upload_) {
            response.state_->upload_->abandoned_ = true;
            response.state_->upload_->output_.stop();
        }

        request.method_.assign(plan.method());
        request.target_.assign(next->target());
        const bool drop = plan.content_disposition() == http_client_redirect_content_disposition::drop;
        std::erase_if(request.headers_, [&](const http_header& field) {
            const auto name = field.name();
            return http_ascii_equals_ignore_case(name, "host") || (next->cross_origin() && (http_ascii_equals_ignore_case(name, "authorization") || http_ascii_equals_ignore_case(name, "proxy-authorization") || http_ascii_equals_ignore_case(name, "cookie"))) || (drop && (http_ascii_equals_ignore_case(name.substr(0, 8), "content-") || http_ascii_equals_ignore_case(name, "digest") || http_ascii_equals_ignore_case(name, "repr-digest") || http_ascii_equals_ignore_case(name, "transfer-encoding") || http_ascii_equals_ignore_case(name, "trailer") || http_ascii_equals_ignore_case(name, "expect")));
        });
        if (drop) {
            request.has_body_ = false;
            std::string(request.body_.get_allocator()).swap(request.body_);
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
    client_.registration_.require_active();
    if (config.max_chunk_bytes_ == 0 || config.continue_timeout_.value() <= std::chrono::milliseconds::zero() || (config.expectation_ != http_client_request_expectation::none && config.expectation_ != http_client_request_expectation::continue_value)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    consumed_ = true;
    return make_scoped_operation(client_.registration_.scope(), upload_owned(std::move(*this), config));
}

task<http_client_exchange> http_client_request::upload_owned(http_client_request request, http_client_upload_config config) {
    std::optional<task<http_client_exchange>> dispatched;
    {
        const detail::inline_http_header_views<16> fields(request.headers_);
        detail::http_client_request_storage storage(request.method_, request.target_, fields.view());
        storage.options_ = detail::copy_http_client_request_settings(request.request_options_);
        storage.informational_ = request.informational_callback_;
        storage.pool_owner_ = request.client_.pool_owner_;
        storage.set_host_stop(request.client_.host_stop_token_);
        dispatched.emplace(request.client_.pool_->open_request(std::move(storage), config, request.client_.options_));
    }
    auto exchange = co_await std::move(*dispatched);
    if (request.max_redirects_ != 0 && exchange.body().state_ != nullptr && exchange.body().state_->upload_) {
        exchange.body().state_->upload_->redirect_request_ = std::make_unique<http_client_request>(std::move(request));
    }
    co_return exchange;
}

http_client_connect_request::http_client_connect_request(const http_client_ref& client, std::string_view authority, std::string_view target, bool udp) :
    client_(client),
    authority_(authority),
    protocol_(),
    target_(target),
    headers_(),
    udp_(udp) {}
http_client_connect_request http_client_ref::connect(std::string_view authority) const {
    registration_.require_active();
    return http_client_connect_request(*this, authority, {}, false);
}
http_client_connect_request http_client_ref::udp(std::string_view path) const {
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
            headers_.emplace_back(field.name(), field.value());
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
    const detail::inline_http_header_views<16> fields(headers_);
    auto dispatched = udp_ ? client_.dispatch_udp({.target_ = target_, .headers_ = fields.view()}, config, request_options_, informational_callback_) : client_.dispatch_connect({.authority_ = authority_, .protocol_ = protocol_, .target_ = target_, .headers_ = fields.view()}, config, request_options_, informational_callback_);
    consumed_ = true;
    std::string(authority_.get_allocator()).swap(authority_);
    std::string(protocol_.get_allocator()).swap(protocol_);
    std::string(target_.get_allocator()).swap(target_);
    std::vector<http_header>(headers_.get_allocator()).swap(headers_);
    return dispatched;
}

#ifdef RUVIA_ENABLE_HTTP3
namespace {
task<void> migrate_client_pool(std::shared_ptr<detail::http_client_pool> pool, std::string address, std::uint16_t port, operation_options options, stop_token host_stop) {
    co_await pool->migrate_http3(std::move(address), port, std::move(options), std::move(host_stop));
}
}  // namespace
#endif

task<void> http_client_ref::migrate(std::string_view local_address, std::uint16_t port) const {
    registration_.require_active();
#ifdef RUVIA_ENABLE_HTTP3
    return make_scoped_operation(registration_.scope(), migrate_client_pool(pool_owner_, std::string(local_address), port, options_, host_stop_token_));
#else
    (void)local_address;
    (void)port;
    throw http_client_error(make_error_code(http_client_errc::protocol_unavailable), "HTTP/3 support is disabled");
#endif
}

namespace {

task<void> refresh_client_dns(std::shared_ptr<detail::http_client_pool> pool, operation_options options, stop_token host_stop) {
    stop_group cancellation(host_stop, options.stop_token_);
    options.stop_token_ = cancellation.token();
    co_await pool->refresh_dns(std::move(options));
}

[[nodiscard]] ip_address to_ruvia_ip_address(const asio::ip::address& address) noexcept {
    if (address.is_v4()) {
        return ip_address::ipv4(address.to_v4().to_bytes());
    }
    const auto v6 = address.to_v6();
    return ip_address::ipv6(v6.to_bytes(), static_cast<std::uint32_t>(v6.scope_id())).normalized_ipv4();
}

[[nodiscard]] task<std::vector<ip_address>> resolve_dns_owned(worker_ref worker, std::string host, operation_options options, std::shared_ptr<const http_client_address_validator> validator = nullptr, std::uint16_t validator_port = 80) {
    const operation_deadline timeout(options.timeout_);
    if (options.stop_token_.stop_requested()) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP DNS resolution cancelled", std::nullopt, nullptr, false);
    }
    if (timeout.expired()) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP DNS resolution timed out", std::nullopt, nullptr, false);
    }
    detail::client_host_resolver resolver(worker);
    auto pending = resolver.resolve(host, validator_port, timeout, options.stop_token_);
    detail::client_resolve_result result;
    if (auto* immediate = std::get_if<detail::client_resolve_result>(&pending)) {
        result = std::move(*immediate);
    } else {
        result = co_await std::move(std::get<task<detail::client_resolve_result>>(pending));
    }
    if (options.stop_token_.stop_requested() || result.error_ == asio::error::operation_aborted) {
        throw http_client_error(std::make_error_code(std::errc::operation_canceled), "HTTP DNS resolution cancelled", std::nullopt, nullptr, false);
    }
    if (timeout.expired() || result.error_ == asio::error::timed_out) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "HTTP DNS resolution timed out", std::nullopt, nullptr, false);
    }
    if (result.error_ == asio::error::no_buffer_space) {
        throw http_client_error(make_error_code(http_client_errc::queue_full), "HTTP DNS resolution blocking pool queue full", std::nullopt, nullptr, false);
    }
    if (result.error_ || (!result.literal_ && result.addresses_.empty())) {
        throw http_client_error(make_error_code(http_client_errc::resolve_failed), "HTTP DNS resolution failed", std::nullopt, nullptr, false);
    }
    std::vector<ip_address> addresses;
    const auto append_endpoint = [&](const asio::ip::tcp::endpoint& endpoint) {
        const auto resolved_ip = to_ruvia_ip_address(endpoint.address());
        if (validator && !(*validator)(resolved_ip, endpoint.port())) {
            throw http_client_error(make_error_code(http_client_errc::resolve_failed), "HTTP dial address rejected", std::nullopt, nullptr, false);
        }
        addresses.push_back(resolved_ip);
    };
    if (result.literal_) {
        append_endpoint(*result.literal_);
    } else {
        addresses.reserve(result.addresses_.size());
        for (const auto& entry : result.addresses_) {
            append_endpoint(entry.endpoint());
        }
    }
    std::ranges::sort(addresses);
    const auto unique_range = std::ranges::unique(addresses);
    addresses.erase(unique_range.begin(), unique_range.end());
    co_return addresses;
}

[[nodiscard]] task<std::vector<ip_address>> resolve_client_handle_dns(std::shared_ptr<detail::http_client_pool> pool, std::string host, operation_options handle_options, operation_options call_options, stop_token host_stop) {
    stop_group cancellation{host_stop, handle_options.stop_token_, call_options.stop_token_};
    std::optional<operation_timeout> effective_timeout = handle_options.timeout_;
    if (call_options.timeout_) {
        if (!effective_timeout || call_options.timeout_->duration() < effective_timeout->duration()) {
            effective_timeout = call_options.timeout_;
        }
    }
    if (!effective_timeout) {
        effective_timeout = operation_timeout(pool->config().connect_timeout_);
    }
    co_return co_await resolve_dns_owned(pool->worker(), std::move(host), {.timeout_ = effective_timeout, .stop_token_ = cancellation.token()}, pool->config().address_validator_, pool->config().dial_port_);
}
}  // namespace

http_client_ref::http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope) noexcept :
    pool_(&pool),
    registration_(scope, this, &http_client_ref::expire_capability) {}

http_client_ref::http_client_ref(detail::http_client_pool& pool, ::ruvia::operation_scope& scope, operation_options options) noexcept :
    pool_(&pool),
    options_(std::move(options)),
    registration_(scope, this, &http_client_ref::expire_capability) {}

http_client_ref::http_client_ref(const http_client_ref& other) :
    pool_(other.pool_),
    pool_owner_(other.pool_owner_),
    options_(other.options_),
    host_stop_token_(other.host_stop_token_),
    registration_(other.registration_, this),
    registry_(other.registry_) {}

void http_client_ref::expire_capability(void* target) noexcept {
    auto& self = *static_cast<http_client_ref*>(target);
    self.pool_ = nullptr;
    self.pool_owner_.reset();
}

http_client_ref http_client_ref::with_options(operation_options options) const {
    registration_.require_active();
    http_client_ref copy(*this);
    copy.options_ = override_operation_options(options_, std::move(options));
    return copy;
}

http_client_ref http_client_ref::bind_caller_scope(::ruvia::operation_scope& scope, const stop_token& host_stop) && noexcept {
    http_client_ref bound(*pool_, scope, std::move(options_));
    bound.pool_owner_ = std::move(pool_owner_);
    bound.registry_ = registry_;
    bound.host_stop_token_ = host_stop;
    return bound;
}

task<http_client_response> http_client_ref::dispatch(const http_client_request_view& view, const http_client_request_options& request_options, const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    detail::http_client_request_storage request(view.method_.view(), view.target_.view(), view.headers_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_host_stop(host_stop_token_);
    if (const auto* bytes = view.content_.borrowed_bytes()) {
        request.set_body(bytes->value());
    }
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->execute(std::move(request), options_));
}

task<http_client_exchange> http_client_ref::dispatch_upload(const http_client_request_view& head, http_client_upload_config upload, const http_client_request_options& request_options, const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (head.content_.borrowed_bytes() != nullptr || upload.max_chunk_bytes_ == 0 || upload.continue_timeout_.value() <= std::chrono::milliseconds::zero() || (upload.expectation_ != http_client_request_expectation::none && upload.expectation_ != http_client_request_expectation::continue_value)) {
        throw std::invalid_argument("streaming upload requires a bodyless head and valid upload policy");
    }
    detail::http_client_request_storage request(head.method_.view(), head.target_.view(), head.headers_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_host_stop(host_stop_token_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_request(std::move(request), upload, options_));
}

task<http_client_tunnel_result> http_client_ref::dispatch_udp(const http_client_udp_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& request_options, const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (!is_valid_http_origin_form_target(head.target_) || config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes) {
        throw std::invalid_argument("invalid CONNECT-UDP request or tunnel policy");
    }
    for (const auto& field : head.headers_) {
        if (http_ascii_equals_ignore_case(field.name(), "capsule-protocol")) {
            throw std::invalid_argument("CONNECT-UDP Capsule-Protocol is driver-owned");
        }
    }
    detail::inline_http_header_views<16> fields(head.headers_.size() + 1U);
    for (const auto& field : head.headers_) {
        fields.push_back(field);
    }
    fields.push_back(http_header_view{"Capsule-Protocol", "?1"});
    // The dispatch owns fields before temporary preparation storage dies.
    const std::string_view authority = request_options.authority_ ? std::string_view(*request_options.authority_) : std::string_view(pool_->config().authority_);
    return dispatch_connect({.authority_ = authority, .protocol_ = "connect-udp", .target_ = head.target_, .headers_ = fields.view()}, config, request_options, informational);
}

task<http_client_tunnel_result> http_client_ref::dispatch_connect(const http_client_tunnel_request_view& head, http_client_tunnel_config config, const http_client_request_options& request_options, const std::shared_ptr<http_client_informational_callback>& informational) const {
    registration_.require_active();
    if (config.max_chunk_bytes_ == 0 || config.max_chunk_bytes_ > default_max_buffered_body_bytes || (head.protocol_.empty() ? (!is_valid_http_connect_authority(head.authority_) || !head.target_.empty()) : (!is_valid_http_method_token(head.protocol_) || !is_valid_http_origin_form_target(head.target_) || !parse_http_authority(head.authority_)))) {
        throw std::invalid_argument("invalid CONNECT request or tunnel policy");
    }
    if (head.protocol_.view() == "connect-udp" && validate_http_connect_udp_request_to_send({.version_ = http_protocol_version::http2, .method_ = "CONNECT", .scheme_ = scheme() == http_scheme::https ? "https" : "http", .authority_ = head.authority_, .path_ = head.target_, .headers_ = head.headers_}).has_value()) {
        throw std::invalid_argument("invalid CONNECT-UDP request head");
    }
    detail::http_client_request_storage request("CONNECT", head.target_.view(), head.headers_);
    request.options_ = detail::copy_http_client_request_settings(request_options);
    request.informational_ = informational;
    request.pool_owner_ = pool_owner_;
    request.set_tunnel(head.authority_.view(), head.protocol_.view());
    request.set_host_stop(host_stop_token_);
    return ::ruvia::make_scoped_operation(registration_.scope(), pool_->open_tunnel(std::move(request), config, options_));
}

http_client_stats http_client_ref::stats() const {
    registration_.require_active();
    return pool_->stats();
}

std::vector<http_client_endpoint_address> http_client_ref::dns_snapshot() const {
    registration_.require_active();
    return pool_->dns_snapshot();
}
void http_client_ref::update_dns(std::span<const http_client_endpoint_address> addresses) {
    registration_.require_active();
    pool_->update_dns(addresses);
}
task<void> http_client_ref::refresh_dns() const {
    registration_.require_active();
    return make_scoped_operation(registration_.scope(), refresh_client_dns(pool_owner_, options_, host_stop_token_));
}
task<std::vector<ip_address>> http_client_ref::resolve_dns(operation_options options) const {
    registration_.require_active();
    if (pool_->config().dial_host_.empty()) {
        throw std::logic_error("HTTP client alias without a fixed origin or dial host has no default DNS target");
    }
    return resolve_dns(pool_->config().dial_host_, std::move(options));
}

task<std::vector<ip_address>> http_client_ref::resolve_dns(std::string_view host, operation_options options) const {
    registration_.require_active();
    if (host.empty() || host.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("DNS host must not be empty or contain NUL");
    }
    detail::require_client_host_lookup(host, pool_->worker());
    return make_scoped_operation(registration_.scope(), resolve_client_handle_dns(pool_owner_, std::string(host), options_, std::move(options), host_stop_token_));
}
bool http_client_ref::close_connection(std::uint64_t id) {
    registration_.require_active();
    return pool_->close_connection(id);
}
std::optional<http_client_push> http_client_ref::next_push() {
    registration_.require_active();
    return pool_->next_push();
}

std::optional<http_client_advertisement> http_client_ref::next_advertisement() {
    registration_.require_active();
    return pool_->next_advertisement();
}

std::string_view http_client_ref::host() const& {
    registration_.require_active();
    return pool_->host();
}

std::uint16_t http_client_ref::port() const {
    registration_.require_active();
    return pool_->port();
}

http_scheme http_client_ref::scheme() const {
    registration_.require_active();
    return pool_->scheme();
}

http_origin_view http_client_ref::origin() const& {
    registration_.require_active();
    return pool_->origin();
}

http_client_ref context::client() const {
    return services().client_registries().client(operation_scope_, capabilities_.stop_token());
}

http_client_ref context::client(std::string_view alias) const {
    return services().client_registries().client(alias, operation_scope_, capabilities_.stop_token());
}

task<std::vector<ip_address>> resolve_dns(const worker_ref& worker, std::string_view host, operation_options options) {
    if (!worker.valid()) {
        throw std::invalid_argument("DNS resolution requires a valid worker handle");
    }
    if (host.empty() || host.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("DNS host must not be empty or contain NUL");
    }
    detail::require_client_host_lookup(host, worker);
    return resolve_dns_owned(worker, std::string(host), std::move(options));
}

task<std::vector<ip_address>> resolve_dns(event_loop loop, std::string_view host, operation_options options) {
    return resolve_dns(loop.worker(), host, std::move(options));
}
}  // namespace ruvia
