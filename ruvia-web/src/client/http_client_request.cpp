#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_ascii.h"
#include "ruvia/http/http_client_redirect.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/web/context.h"
#include "ruvia/web/http_client.h"
#include "ruvia/web/streaming.h"

#include "client/http_client_pool.h"
#include "client/http_client_registry.h"
#include "client/http_client_response_state.h"

namespace ruvia {
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

task<void> pipe_response(http_client_response response, task<void> body, response_stream_writer& output) {
    if (co_await output.commit() == stream_commit_result::body) {
        co_await std::move(body);
        std::pmr::vector<http_header_view> trailers(process_resource());
        trailers.reserve(response.trailers().size());
        for (const auto& field : response.trailers()) {
            if (!hop_field(field.name()) && !connection_named_field(response.headers(), field.name())) {
                trailers.emplace_back(field.name(), field.value());
            }
        }
        co_await output.end(trailers);
    }
}

// A task frame may bind exactly one scope. This outer frame binds the
// destination while its child remains bound to the source response.
task<void> pipe_response_in_context(task<void> source) {
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
    for (const auto& field : fields) {
        headers_.emplace_back(field.name(), field.value(), headers_.get_allocator().resource());
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
    std::pmr::string suffix(target_.get_allocator().resource());
    suffix.push_back(target_.find('?') == std::string_view::npos ? '?' : '&');
    append_query_component(suffix, name);
    suffix.push_back('=');
    append_query_component(suffix, value);
    target_.append(suffix);
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
std::pmr::vector<http_header_view> http_client_request::field_views() const {
    std::pmr::vector<http_header_view> fields(headers_.get_allocator().resource());
    fields.reserve(headers_.size());
    for (const auto& field : headers_) {
        fields.emplace_back(field.name(), field.value());
    }
    return fields;
}

task<http_client_response> http_client_request::send() {
    if (std::exchange(consumed_, true)) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    client_.registration_.require_active();
    return make_scoped_operation(client_.registration_.scope(), send_owned(std::move(*this)));
}

task<http_client_response> http_client_request::send_owned(http_client_request request) {
    const auto started = std::chrono::steady_clock::now();
    const auto total = request.client_.options_.timeout_.has_value() ? request.client_.options_.timeout_ : request.client_.pool_->config().request_timeout_;
    for (std::size_t redirects = 0;; ++redirects) {
        request.client_.registration_.require_active();
        if (total) {
            const auto elapsed = std::chrono::ceil<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            if (elapsed >= *total) {
                throw http_client_error(http_client_error::code_type::timeout, "HTTP redirect deadline expired");
            }
            request.client_.options_.timeout_ = *total - elapsed;
        }
        auto fields = request.field_views();
        http_client_request_view view{.method_ = request.method_, .target_ = request.target_, .headers_ = fields, .content_ = request.has_body_ ? http_client_request_content_view::bytes(request.body_) : http_client_request_content_view::none()};
        auto response = co_await request.client_.dispatch(view);
        const auto status = response.status().value();
        if (redirects == request.max_redirects_ ||
            (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)) {
            co_return response;
        }
        std::optional<std::string_view> location;
        for (const auto& field : response.headers()) {
            if (http_ascii_equals_ignore_case(field.name(), "location")) {
                if (location) {
                    throw http_client_error(http_client_error::code_type::protocol_error, "repeated redirect Location");
                }
                location = field.value();
            }
        }
        if (!location) {
            co_return response;
        }
        auto resolved = resolve_http_client_redirect_target(request.client_.origin(),
            {.current_target_ = request.target_, .location_ = *location, .resource_ = request.client_.resource_});
        auto* next = std::get_if<http_client_resolved_redirect>(&resolved);
        if (next == nullptr) {
            co_return response;
        }
        auto* next_pool = request.client_.pool_;
        if (next->cross_origin()) {
            if (request.client_.scheme() == http_scheme::https && next->scheme() != http_scheme::https) {
                co_return response;
            }
            // Redirects cannot expand startup's configured destination set.
            if (request.client_.registry_ == nullptr) {
                co_return response;
            }
            auto authority = make_http_origin_authority(next->origin(), request.client_.resource_);
            next_pool = request.client_.registry_->find_origin(next->scheme(), authority);
            if (next_pool == nullptr) {
                co_return response;
            }
        }
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
            request.body_.clear();
        }
        request.client_.pool_ = next_pool;
    }
}

task<http_client_exchange> http_client_request::upload(http_client_upload_config config) {
    if (std::exchange(consumed_, true)) {
        throw std::logic_error("HTTP request builder was consumed");
    }
    if (has_body_) {
        throw std::invalid_argument("streamed upload cannot also have a buffered body");
    }
    const auto fields = field_views();
    return client_.dispatch_upload({.method_ = method_, .target_ = target_, .headers_ = fields}, config);
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
    for (const auto& field : fields) {
        headers_.emplace_back(field.name(), field.value(), headers_.get_allocator().resource());
    }
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
    if (std::exchange(consumed_, true)) {
        throw std::logic_error("CONNECT builder was consumed");
    }
    std::pmr::vector<http_header_view> fields(headers_.get_allocator().resource());
    fields.reserve(headers_.size());
    for (const auto& field : headers_) {
        fields.emplace_back(field.name(), field.value());
    }
    if (udp_) {
        return client_.dispatch_udp({.target_ = target_, .headers_ = fields}, config);
    }
    return client_.dispatch_connect({.authority_ = authority_, .protocol_ = protocol_, .target_ = target_, .headers_ = fields}, config);
}

task<void> http_client_response::pipe_to(context& output) & {
    output.draft().status(status());
    const auto fields = headers();
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        if (!hop_field(field->name()) && !connection_named_field(fields, field->name()) &&
            !http_ascii_equals_ignore_case(field->name(), "content-length")) {
            const bool repeated = std::any_of(fields.begin(), field, [&](const http_header& previous) {
                return http_ascii_equals_ignore_case(previous.name(), field->name());
            });
            output.draft().header(field->name(), field->value(),
                {.mode_ = repeated ? http_response_header_mode::append : http_response_header_mode::replace});
        }
    }
    auto& writer = output.stream();
    auto source = make_scoped_operation(state_->body_operation_scope_,
        pipe_response(http_client_response(state_), body_.pipe_to(writer), writer));
    return make_scoped_operation(output.operation_scope_,
        pipe_response_in_context(std::move(source)));
}

task<void> http_client_handle::migrate(std::string_view local_address, std::uint16_t port) const {
    registration_.require_active();
#ifdef RUVIA_ENABLE_HTTP3
    return make_scoped_operation(registration_.scope(), pool_->migrate_http3(local_address, port));
#else
    (void)local_address;
    (void)port;
    throw http_client_error(http_client_error::code_type::protocol_unavailable, "HTTP/3 support is disabled");
#endif
}
}  // namespace ruvia
