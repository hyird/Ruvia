#include "ruvia/web/server_connection.h"

#include <exception>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_timeout.h"

#include "http/tls_tunnel_output.h"
#include "server/inbound_buffer_resource.h"
#include "server/peer_address.h"
#include "server/server_connection_session.h"
#include "server/server_tls_stream.h"
#include "server/worker_connections.h"
#include "util/operation_lane_lease.h"

namespace ruvia {
namespace {

constexpr const char* connection_lane_in_progress = "server connection operation already in progress on this direction";

task<std::optional<std::span<const std::byte>>> read_connection(void* target,
    task<std::optional<std::span<const std::byte>>> (*read)(void*, operation_options), bool& read_active,
    operation_options operation) {
    auto lease = detail::claim_operation_lane(read_active, connection_lane_in_progress);
    co_return co_await read(target, std::move(operation));
}

task<void> write_connection(void* target, task<void> (*write)(void*, std::string_view, operation_options),
    std::pmr::string bytes, bool& write_active, const bool& send_ended, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, connection_lane_in_progress);
    if (send_ended) {
        throw std::logic_error("server connection send direction has ended");
    }
    co_await write(target, bytes, std::move(operation));
}

task<void> finish_connection(void* target, task<void> (*finish_value)(void*, operation_options), bool& send_ended,
    bool& write_active, operation_options operation) {
    auto lease = detail::claim_operation_lane(write_active, connection_lane_in_progress);
    if (!send_ended) {
        co_await finish_value(target, std::move(operation));
        send_ended = true;
    }
}

}  // namespace

server_connection::server_connection(const detail::server_connection_fields& fields,
    const ::ruvia::stop_token& stop_token_value, const worker_handle& worker_value, void* target, read_type read,
    write_type write, finish_type finish_value, abort_type abort_value) noexcept
    : worker_(worker_value),
      target_(target),
      read_(read),
      write_(write),
      finish_(finish_value),
      abort_(abort_value),
      id_(fields.id_),
      local_(fields.local_address_, fields.local_port_),
      remote_(fields.remote_address_, fields.remote_port_),
      stop_token_(stop_token_value),
      state_(fields.state_) {
    if (fields.tls_.has_value()) {
        tls_.emplace(tls_connection_transport(
            fields.tls_->client_certificate_subject_, fields.tls_->alpn_, fields.tls_->server_name_));
    }
    operations_.bind_worker(worker_);
}

void server_connection::require_active() const {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!operations_.active()) {
        throw std::logic_error("server connection has expired");
    }
}

task<std::optional<std::span<const std::byte>>> server_connection::read(operation_options operation) & {
    require_active();
    validate_operation_options(operation);
    return ::ruvia::make_scoped_operation(operations_, read_connection(target_, read_, read_active_, std::move(operation)));
}

task<void> server_connection::write(std::span<const std::byte> bytes, operation_options operation) & {
    require_active();
    validate_operation_options(operation);
    if (send_ended_) {
        throw std::logic_error("server connection send direction has ended");
    }
    return ::ruvia::make_scoped_operation(operations_,
        write_connection(target_, write_, std::pmr::string(as_chars(bytes), process_resource()), write_active_,
            send_ended_, std::move(operation)));
}

task<void> server_connection::finish(operation_options operation) & {
    require_active();
    validate_operation_options(operation);
    return ::ruvia::make_scoped_operation(
        operations_, finish_connection(target_, finish_, send_ended_, write_active_, std::move(operation)));
}

void server_connection::abort() noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (operations_.active()) {
        abort_(target_);
    }
}

}  // namespace ruvia

namespace ruvia::detail {

// The handler owns the connection until its task completes; the connection
// then ends here, so a handler never has to close it itself. The TLS ciphertext
// writer outlives the session that writes through it and is joined on every
// exit.
template <typename stream_type>
task<void> worker_connections::serve_connection(
    stream_type& stream, tcp_socket_type& socket, server_connection_fields fields) {
    // A stopping worker has already closed the connection.
    if (!http_server_worker_running(state_) || !socket.is_open()) {
        co_return;
    }
    std::pmr::string local_address(process_resource());
    std::error_code local_error;
    if (const auto local = socket.local_endpoint(local_error); !local_error) {
        assign_peer_address(local_address, local.address());
        fields.local_address_ = local_address;
        fields.local_port_ = local.port();
    }
    connection_scanner::entry scanner_entry;
    connection_scanner::guard scanner_guard(scanner_, scanner_entry, connection_transport::tcp(socket));
    inbound_buffer_resource connection_buffers(
        *options_.inbound_budget_, options_.server_.max_inbound_buffer_bytes_per_connection_);
    std::optional<tls_tunnel_output> tls_output;
    if constexpr (requires { stream.next_layer(); }) {
        tls_output.emplace(*stream.native_handle(), socket, worker_, *process_resource());
        tls_output->start();
    }
    std::exception_ptr failure;
    {
        server_connection_session<stream_type> session(stream, tls_output.has_value() ? &*tls_output : nullptr,
            scanner_entry, connection_buffers, worker_, stop_token_, fields);
        try {
            co_await options_.connection_(session.connection());
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure != nullptr) {
            session.abort();
        }
        co_await session.join();
        // A worker stop keeps the immediate close.
        if (failure == nullptr && !session.aborted() && !stop_token_.stop_requested()) {
            try {
                co_await session.finish();
            } catch (...) {
                failure = std::current_exception();
            }
        }
    }
    if (tls_output.has_value()) {
        if (failure != nullptr) {
            tls_output->abort();
        }
        co_await tls_output->join();
    }
    if (failure != nullptr) {
        std::rethrow_exception(failure);
    }
}

template task<void> worker_connections::serve_connection<tcp_socket_type>(
    tcp_socket_type&, tcp_socket_type&, server_connection_fields);
template task<void> worker_connections::serve_connection<server_tls_stream>(
    server_tls_stream&, tcp_socket_type&, server_connection_fields);

}  // namespace ruvia::detail
