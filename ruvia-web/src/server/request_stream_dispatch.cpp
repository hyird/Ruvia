#include "server/request_stream_dispatch.h"

#include <chrono>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/core/timer.h"
#include "ruvia/http/http1_server_semantics.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_known_method.h"
#include "ruvia/http/http_response.h"
#include "ruvia/web/streaming.h"

#include "context/context_access.h"
#include "context/context_services.h"
#include "http/streaming_access.h"
#include "router/compiled_route_plan.h"
#include "router/route_table.h"
#include "server/http_response_stream_dispatch.h"
#include "server/http_server_options.h"

namespace ruvia::detail {

struct request_stream_access final {
    [[nodiscard]] static request_stream make(context& context_value, response_stream_writer& response,
        const request_stream_tunnel_accept* accept_tunnel) noexcept {
        return request_stream(context_value, response, accept_tunnel);
    }
};

namespace {

// Runs the connection's callback; returns its failure, if any.
task<std::exception_ptr> run_request_stream_callback(
    context& context_value, response_stream_writer& response, const request_stream_tunnel_accept* accept_tunnel) {
    const auto callback = context_access::services(context_value).request_stream();
    auto stream = request_stream_access::make(context_value, response, accept_tunnel);
    try {
        co_await callback(stream);
    } catch (...) {
        co_return std::current_exception();
    }
    co_return nullptr;
}

// After the callback: a committed response or tunnel stands, and a failure
// past it propagates to the transport, which aborts the stream and reports it.
// Otherwise nothing reached the peer: the request gets the framework's answer
// to the failure -- a callback that committed nothing is a fault -- and an
// unexpected one reaches the connection failure sink. No application error
// handler takes part.
void complete_request_stream(context& context_value, std::exception_ptr failure) {
    if (context_access::output_committed(context_value)) {
        if (failure != nullptr) {
            std::rethrow_exception(failure);
        }
        return;
    }
    if (failure == nullptr) {
        if (context_access::has_response(context_value)) {
            return;
        }
        failure = std::make_exception_ptr(std::logic_error("request stream callback completed without a response"));
    }
    auto answer = make_default_exception_response(context_value.arena(), failure);
    if (answer.unexpected_) {
        context_access::services(context_value)
            .connection_failure()
            .invoke(context_value.conn().remote().address(), failure);
    }
    context_access::set_response(context_value, std::move(answer.response_));
}

// The response route of a request stream connection: the callback writes
// through the transport's response stream, bound to the context.
task<void> serve_request_stream(void*, context& context_value) {
    auto failure = co_await run_request_stream_callback(context_value, context_value.stream(), nullptr);
    complete_request_stream(context_value, std::move(failure));
}

// What a tunnel request's callback writes instead of accepting the tunnel.
// Nothing reaches the peer before the callback returns, so the response is
// kept whole and handed back as the context's buffered response; it carries
// no trailers.
class request_stream_refusal final {
public:
    explicit request_stream_refusal(context& context_value)
        : context_(context_value),
          body_(context_value.arena()) {}

    request_stream_refusal(const request_stream_refusal&) = delete;
    request_stream_refusal& operator=(const request_stream_refusal&) = delete;

    task<stream_commit_result> commit() {
        if (head_.has_value()) {
            throw std::logic_error("response stream is already committed");
        }
        // accept_tunnel() committed the upgrade: no response can follow.
        if (context_access::output_committed(context_)) {
            throw std::logic_error("the request's tunnel is already established");
        }
        auto head = context_access::streaming_head(context_);
        const auto method = context_access::request(context_).known_method();
        // RFC 9110 §9.3.6: any 2xx response to CONNECT switches to tunnel mode.
        if (method == http_known_method::connect && head.status().is_successful()) {
            throw std::logic_error("a 2xx response to CONNECT establishes the tunnel; use accept_tunnel()");
        }
        ended_ = plan_http_response_body(method, head.status()).body_suppressed();
        head_.emplace(std::move(head));
        co_return ended_ ? stream_commit_result::completed : stream_commit_result::body;
    }

    task<void> write(std::string_view chunk) {
        require_body();
        body_.append(chunk);
        co_return;
    }

    task<void> end(std::span<const http_header_view> trailers) {
        if (!trailers.empty()) {
            throw std::logic_error(ended_ ? "response stream is already ended"
                                          : "a refused tunnel request's response carries no trailers");
        }
        if (!head_.has_value()) {
            (void)co_await commit();
        }
        ended_ = true;
    }

    task<timer_sleep_result> sleep(std::chrono::milliseconds duration, const stop_token& stop_token_value) {
        require_body();
        co_return co_await sleep_for(context_.worker(), duration, stop_token_value);
    }

    void bind_context(context*) noexcept {}

    void release_context() noexcept {}

    [[nodiscard]] bool committed() const noexcept {
        return head_.has_value();
    }

    [[nodiscard]] bool aborted() const noexcept {
        return false;
    }

    // Only once committed().
    [[nodiscard]] http_response take_response() {
        head_->owned_body(std::move(body_));
        return std::move(*head_);
    }

private:
    void require_body() const {
        if (!head_.has_value() || ended_) {
            throw std::logic_error("response stream has no open body");
        }
    }

    context& context_;
    std::optional<http_response> head_;
    std::pmr::string body_;
    bool ended_{false};
};

[[nodiscard]] const route_entry& request_stream_response_route() noexcept {
    // Process-invariant and allocation-free: no route path, method token or
    // protocol to own.
    static const route_entry route(process_resource(),
        {.method_ = http_known_method::unknown,
            .path_ = {},
            .endpoint_ = route_endpoint::response_stream(&serve_request_stream, request_body_mode::stream)});
    return route;
}

[[nodiscard]] const route_entry& request_stream_tunnel_route() noexcept {
    // The tunnel upgrade runs serve_request_stream_tunnel() for this route
    // instead of its handler.
    static const route_entry route(process_resource(),
        {.method_ = http_known_method::unknown,
            .path_ = {},
            .endpoint_ = route_endpoint::tunnel(process_resource(), &serve_request_stream, {}, {})});
    return route;
}

}  // namespace

route_resolution resolve_request_stream_route(bool tunnel) noexcept {
    return route_resolution::resolved(tunnel ? request_stream_tunnel_route() : request_stream_response_route(), nullptr);
}

bool is_request_stream_tunnel_route(const route_entry& route) noexcept {
    return &route == &request_stream_tunnel_route();
}

bool http1_request_stream_tunnel(const http_request& request, bool content) noexcept {
    return request.known_method() == http_known_method::connect || (!content && http1_request_offers_upgrade(request));
}

task<void> serve_request_stream_tunnel(context& context_value, const request_stream_tunnel_accept& accept) {
    request_stream_refusal refusal(context_value);
    auto response = make_response_stream_writer(refusal, *process_resource());
    auto& services = context_access::services(context_value);
    services.bind_response_stream(response);
    streaming_access::bind_context(response, context_value, context_value.stop_token());
    auto failure = co_await run_request_stream_callback(context_value, response, &accept);
    streaming_access::release_context(response);
    // Unless accept_tunnel() committed the upgrade, nothing reached the peer:
    // the writer leaves the context, whose output is a buffered response again.
    if (services.response_output().response_stream() != nullptr) {
        services.response_output() = context_response_output();
    }
    if (failure == nullptr && refusal.committed()) {
        try {
            context_access::set_response(context_value, refusal.take_response());
        } catch (...) {
            failure = std::current_exception();
        }
    }
    complete_request_stream(context_value, std::move(failure));
}

}  // namespace ruvia::detail

namespace ruvia {

const http_request& request_stream::request() const& noexcept {
    return detail::context_access::request(context_);
}

const conn_info& request_stream::conn() const& noexcept {
    return context_.conn();
}

body_reader& request_stream::body() & {
    if (accept_tunnel_ != nullptr) {
        throw std::logic_error("a tunnel request carries no content: its bytes belong to accept_tunnel()");
    }
    return context_.req().body_reader();
}

context_response_editor request_stream::draft() & noexcept {
    return context_.draft();
}

response_stream_writer& request_stream::response() & noexcept {
    return response_;
}

bool request_stream::tunnel_requested() const noexcept {
    return accept_tunnel_ != nullptr;
}

task<http_tunnel*> request_stream::accept_tunnel(http_status_code status) & {
    if (accept_tunnel_ == nullptr) {
        throw std::logic_error("accept_tunnel() requires a CONNECT or HTTP/1.1 Upgrade request");
    }
    if (response_.committed() || detail::context_access::output_committed(context_)) {
        throw std::logic_error("the request's response is already committed");
    }
    if (request().known_method() == http_known_method::connect) {
        if (!status.is_successful()) {
            throw std::invalid_argument("a CONNECT tunnel is accepted with a 2xx status");
        }
        context_.draft().status(status);
    } else if (status != http_status::switching_protocols) {
        throw std::invalid_argument("an HTTP/1.1 Upgrade is accepted with 101 (Switching Protocols)");
    }
    co_return co_await (*accept_tunnel_)(status);
}

::ruvia::stop_token request_stream::stop_token() const noexcept {
    return context_.stop_token();
}

}  // namespace ruvia
