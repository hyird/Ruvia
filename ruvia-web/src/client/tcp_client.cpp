#include "ruvia/web/tcp_client.h"

#include <array>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <asio/ssl/context.hpp>

#include "ruvia/core/bytes.h"
#include "ruvia/core/event_loop.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/worker_cancellation.h"

#include "client/client_lifecycle.h"
#include "client/client_tcp_transport.h"
#include "client/client_tls.h"
#include "util/config_validation.h"
#include "util/operation_lane_lease.h"
#include "util/pmr_owned_storage.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] std::pmr::string owned_host(const tcp_client_config& config) {
    validate_client_origin_host(config.host_, "TCP host must not be empty", "TCP host is invalid");
    if (config.port_.value() == 0) {
        throw std::invalid_argument("TCP port must not be zero");
    }
    validate_client_tls_config(config.tls_);
    validate_client_tcp_config(config.tcp_);
    for (const auto& timeout : {config.connect_timeout_, config.read_timeout_, config.write_timeout_}) {
        if (timeout.has_value()) {
            // Each configured bound becomes an operation_deadline, which rejects non-positive values.
            static_cast<void>(operation_timeout(*timeout));
        }
    }
    return std::pmr::string(config.host_, process_resource());
}

[[nodiscard]] std::pmr::vector<unsigned char> owned_alpn(const tcp_client_config& config) {
    if (!config.alpn_.empty() && config.tls_.mode_ == client_tls_mode::disabled) {
        throw std::invalid_argument("TCP ALPN requires TLS");
    }
    std::size_t size = 0;
    for (const auto& protocol : config.alpn_) {
        if (protocol.empty() || protocol.size() > 255) {
            throw std::invalid_argument("TLS ALPN names must contain 1..255 bytes");
        }
        if (65535 - size < protocol.size() + 1) {
            throw std::invalid_argument("TLS ALPN offer exceeds 65535 bytes");
        }
        size += protocol.size() + 1;
    }
    std::pmr::vector<unsigned char> wire(process_resource());
    wire.reserve(size);
    for (const auto& protocol : config.alpn_) {
        wire.push_back(static_cast<unsigned char>(protocol.size()));
        const auto* bytes = reinterpret_cast<const unsigned char*>(protocol.data());
        wire.insert(wire.end(), bytes, bytes + protocol.size());
    }
    return wire;
}

[[nodiscard]] asio::ssl::context make_tls_context(const client_tls_config_storage& config) {
    asio::ssl::context context(asio::ssl::context::tls_client);
    if (config.mode_ != client_tls_mode::disabled) {
        configure_client_tls_context(*context.native_handle(), config);
    }
    return context;
}

[[noreturn]] void throw_transport_error(const client_transport_result& result) {
    std::error_code error = result.error_;
    if (result.status_ == client_transport_status::aborted) {
        error = std::make_error_code(result.abort_ == client_abort_reason::timeout ? std::errc::timed_out
                                                                                   : std::errc::operation_canceled);
    }
    if (!error) {
        error = std::make_error_code(std::errc::io_error);
    }
    throw std::system_error(error, "TCP client operation failed");
}
}  // namespace

class tcp_client_backend final {
public:
    tcp_client_backend(event_loop& loop, const worker_handle& worker, const tcp_client_config& config)
        : host_(owned_host(config)),
          alpn_(owned_alpn(config)),
          tls_config_(config.tls_, client_tls_files::parse, process_resource()),
          tls_context_(make_tls_context(tls_config_)),
          transport_(loop.io_context(), tls_context_, worker,
              {host_, config.port_.value(), tls_config_.mode_ == client_tls_mode::disabled ? nullptr : &tls_config_, config.tcp_, alpn_}, counters_),
          cancellation_(make_worker_cancellation_target(*this, worker)),
          connect_timeout_(config.connect_timeout_),
          read_timeout_(config.read_timeout_),
          write_timeout_(config.write_timeout_) {}

    ~tcp_client_backend() {
        cancellation_->detach(*this);
    }

    [[nodiscard]] task<void> connect() {
        // The lifecycle admits one startup; snapshot options before suspending.
        return connect_transport(std::move(connect_options_));
    }

    [[nodiscard]] std::string_view alpn() noexcept {
        return transport_.alpn();
    }

    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options options) {
        auto lease = claim_operation_lane(read_active_, "TCP client read already in progress");
        operation_deadline timeout(override_operation_options({.timeout_ = read_timeout_}, options).timeout_);
        worker_cancellation_registration cancellation(cancellation_, read_id_);
        cancellation.arm(options.stop_token_);
        if (transport_.abort_reason() != client_abort_reason::none) {
            throw_transport_error({.status_ = client_transport_status::aborted, .abort_ = transport_.abort_reason()});
        }
        if (eof_) {
            co_return std::nullopt;
        }
        const auto result = co_await transport_.read_some(buffer_, timeout);
        if (result.status_ == client_transport_status::eof) {
            eof_ = true;
            co_return std::nullopt;
        }
        if (!result.ok()) {
            throw_transport_error(result);
        }
        co_return as_bytes(std::string_view(buffer_.data(), result.bytes_));
    }

    [[nodiscard]] task<void> write(std::pmr::string bytes, operation_options options) {
        auto lease = claim_operation_lane(write_active_, "TCP client write already in progress");
        operation_deadline timeout(override_operation_options({.timeout_ = write_timeout_}, options).timeout_);
        worker_cancellation_registration cancellation(cancellation_, write_id_);
        cancellation.arm(options.stop_token_);
        const auto result = co_await transport_.write(bytes, timeout);
        if (!result.ok()) {
            throw_transport_error(result);
        }
    }

    void close_now() noexcept {
        transport_.abort(client_abort_reason::closing);
        transport_.close();
    }

    void cancel_operation_by_id(std::uint64_t id) noexcept {
        if (id != 0 && (id == connect_id_ || id == read_id_ || id == write_id_)) {
            transport_.abort(client_abort_reason::cancelled);
            // A composed plaintext write may have a successful completion
            // queued but no socket wait for cancel() to interrupt. Cancellation
            // retires this connection, so close it before that completion can
            // initiate another write, and cancel both lane deadlines as well.
            transport_.close();
        }
    }

    operation_options connect_options_;

private:
    [[nodiscard]] task<void> connect_transport(operation_options options) {
        operation_deadline timeout(override_operation_options({.timeout_ = connect_timeout_}, options).timeout_);
        worker_cancellation_registration cancellation(cancellation_, connect_id_);
        cancellation.arm(options.stop_token_);
        const auto result = co_await transport_.connect(timeout);
        if (!result.ok()) {
            throw_transport_error(result);
        }
    }

    std::pmr::string host_;
    std::pmr::vector<unsigned char> alpn_;
    client_tls_config_storage tls_config_;
    asio::ssl::context tls_context_;
    client_wire_counters counters_;
    client_tcp_transport transport_;
    std::shared_ptr<worker_cancellation_target<tcp_client_backend>> cancellation_;
    std::optional<timeout_duration> connect_timeout_;
    std::optional<timeout_duration> read_timeout_;
    std::optional<timeout_duration> write_timeout_;
    std::array<char, 4096> buffer_{};
    std::uint64_t connect_id_{};
    std::uint64_t read_id_{};
    std::uint64_t write_id_{};
    bool read_active_{};
    bool write_active_{};
    bool eof_{};
};

class tcp_client_state final : public std::enable_shared_from_this<tcp_client_state> {
public:
    tcp_client_state(event_loop loop, const tcp_client_config& config)
        : loop_(client_lifecycle<tcp_client_state>::require_loop(std::move(loop))),
          worker_(loop_.handle()),
          backend_(loop_, worker_, config),
          lifecycle_(*this, loop_, worker_) {}

    void bind_stop() {
        lifecycle_.bind_stop();
    }
    [[nodiscard]] task<void> connect(operation_options options) {
        return connect_owned(shared_from_this(), std::move(options));
    }
    [[nodiscard]] std::string_view alpn() {
        lifecycle_.require_ready();
        return backend_.alpn();
    }
    [[nodiscard]] task<std::optional<std::span<const std::byte>>> read(operation_options options) {
        lifecycle_.require_ready();
        return make_scoped_operation(lifecycle_.operation_scope(), read_owned(shared_from_this(), std::move(options)));
    }
    [[nodiscard]] task<void> write(std::pmr::string bytes, operation_options options) {
        lifecycle_.require_ready();
        return make_scoped_operation(lifecycle_.operation_scope(), write_owned(shared_from_this(), std::move(bytes), std::move(options)));
    }
    void request_close() noexcept {
        lifecycle_.request_close();
    }
    [[nodiscard]] task<void> shutdown() {
        return lifecycle_.shutdown();
    }
    [[nodiscard]] const worker_handle& worker() const noexcept {
        return worker_;
    }

private:
    friend class client_lifecycle<tcp_client_state>;
    static constexpr std::string_view client_name{"TCP"};
    [[noreturn]] static void throw_not_ready() {
        throw std::system_error(std::make_error_code(std::errc::not_connected), "TCP client is not connected");
    }
    [[nodiscard]] tcp_client_backend& backend() noexcept {
        return backend_;
    }
    [[nodiscard]] static task<void> connect_owned(std::shared_ptr<tcp_client_state> state, operation_options options) {
        if (!state->worker_.is_current()) {
            throw std::logic_error("TCP client must connect on its bound event loop");
        }
        auto lease = claim_operation_lane(state->connecting_, "TCP client connect already in progress");
        state->backend_.connect_options_ = std::move(options);
        co_await state->lifecycle_.connect();
    }
    [[nodiscard]] static task<std::optional<std::span<const std::byte>>> read_owned(
        std::shared_ptr<tcp_client_state> state, operation_options options) {
        state->lifecycle_.require_ready();
        // Lane admission errors are caller errors, not transport failures.
        try {
            co_return co_await state->backend_.read(std::move(options));
        } catch (const std::system_error&) {
            state->request_close();
            throw;
        }
    }
    [[nodiscard]] static task<void> write_owned(std::shared_ptr<tcp_client_state> state,
        std::pmr::string bytes, operation_options options) {
        state->lifecycle_.require_ready();
        try {
            co_await state->backend_.write(std::move(bytes), std::move(options));
        } catch (const std::system_error&) {
            state->request_close();
            throw;
        }
    }

    event_loop loop_;
    worker_handle worker_;
    tcp_client_backend backend_;
    bool connecting_{};
    client_lifecycle<tcp_client_state> lifecycle_;
};

}  // namespace ruvia::detail

namespace ruvia {

tcp_client::tcp_client(event_loop loop, const tcp_client_config& config)
    : state_(std::allocate_shared<detail::tcp_client_state>(
          std::pmr::polymorphic_allocator<detail::tcp_client_state>(process_resource()), std::move(loop), config)) {
    state_->bind_stop();
}

tcp_client::~tcp_client() {
    state_->request_close();
}

task<void> tcp_client::connect(operation_options options) & {
    return state_->connect(std::move(options));
}

std::string_view tcp_client::alpn() const& {
    return state_->alpn();
}
task<std::optional<std::span<const std::byte>>> tcp_client::read(operation_options options) & {
    return state_->read(std::move(options));
}

task<void> tcp_client::write(std::span<const std::byte> bytes, operation_options options) & {
    return write_owned(std::pmr::string(as_chars(bytes), process_resource()), std::move(options));
}

task<void> tcp_client::write(std::string_view bytes, operation_options options) & {
    return write_owned(std::pmr::string(bytes, process_resource()), std::move(options));
}

task<void> tcp_client::write_owned(std::pmr::string bytes, operation_options options) {
    return state_->write(detail::rebind_owned_storage(bytes, process_resource()), std::move(options));
}

void tcp_client::close() noexcept {
    state_->request_close();
}

task<void> tcp_client::shutdown() & {
    return state_->shutdown();
}

const worker_handle& tcp_client::worker() const& noexcept {
    return state_->worker();
}

}  // namespace ruvia
