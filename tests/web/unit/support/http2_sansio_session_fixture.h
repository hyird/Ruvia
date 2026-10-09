#pragma once

#include <string_view>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/connection_scanner.h"
#include "ruvia/core/event_loop_attachment.h"

#include "context/context_services.h"
#include "http2/http2_sans_io_session.h"

namespace ruvia::test {

// Test-only owner for the production session's mandatory connection wiring.
// Keeping these defaults here prevents test convenience from weakening the
// installed runtime contract.
class http2_sans_io_session_fixture final {
public:
    [[nodiscard]] detail::context_services services(const worker_handle& worker_value) const {
        return detail::context_services(worker_value, stop_token_);
    }

    [[nodiscard]] detail::http2_sans_io_session_context context(detail::context_services services) {
        return detail::http2_sans_io_session_context(
            std::move(services), options_, scanner_entry_, worker_state_);
    }

    detail::http_server_options options_;
    connection_scanner::entry_type scanner_entry_;
    detail::http_server_worker_state worker_state_{detail::http_server_worker_state::running};

private:
    stop_token stop_token_;
};

template <typename stream_type, typename bind_transport_type>
task<void> run_bare_http2_sans_io_session_with(stream_type& stream, const detail::route_table& routes_value,
    worker_memory& worker_value, bind_transport_type bind_transport, std::string_view initial_bytes) {
    http2_sans_io_session_fixture fixture;
    auto attachment = attach_event_loop(
        static_cast<asio::io_context&>(stream.get_executor().context()), {.queue_capacity_ = 64});
    const auto worker_handle_value = attachment.loop().handle();
    auto services = bind_transport(fixture.services(worker_handle_value));
    co_await detail::run_http2_sans_io_session(
        stream, routes_value, worker_value, fixture.context(services), initial_bytes);
}

// Convenience for the many cleartext socket tests. TLS tests must call the
// typed helper above so the stream type cannot silently manufacture identity.
template <typename stream_type>
task<void> run_bare_plain_http2_sans_io_session(stream_type& stream, const detail::route_table& routes_value,
    worker_memory& worker_value, std::string_view remote_address, std::string_view initial_bytes = {}) {
    co_await run_bare_http2_sans_io_session_with(
        stream, routes_value, worker_value,
        [remote_address](detail::context_services services) {
            return services.with_plain_transport(remote_address);
        },
        initial_bytes);
}

template <typename stream_type>
task<void> run_bare_tls_http2_sans_io_session(stream_type& stream, const detail::route_table& routes_value,
    worker_memory& worker_value, std::string_view remote_address,
    std::string_view client_certificate_subject = {}, std::string_view initial_bytes = {}) {
    co_await run_bare_http2_sans_io_session_with(
        stream, routes_value, worker_value,
        [remote_address, client_certificate_subject](detail::context_services services) {
            return services.with_tls_transport(remote_address, client_certificate_subject);
        },
        initial_bytes);
}

}  // namespace ruvia::test
