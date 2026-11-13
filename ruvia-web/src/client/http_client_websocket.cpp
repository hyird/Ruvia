#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "ruvia/core/bytes.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/core/operation_deadline.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/web/http_client_websocket_request.h"

#include "client/http_client_pool.h"
#include "client/http_client_request_storage.h"
#include "client/http_client_response_state.h"
#include "http/inline_http_header_views.h"
#include "tls/tls_random.h"
#include "websocket/http_websocket_connection.h"
#include "websocket/websocket_facade.h"

namespace ruvia::detail {

// Only adapts tunnel I/O. All WebSocket framing, lanes, control traffic,
// interruption and close policy live in websocket_session_connection.
class http_client_websocket_transport final {
public:
    http_client_websocket_transport(http_client_tunnel tunnel, connection_scanner::entry& activity)
        : tunnel_(std::move(tunnel)),
          response_(*tunnel_.response_.state_),
          activity_(activity) {}

    template <typename check_type>
    task<bool> read_more(http_io_buffer& bytes, check_type check, asio::cancellation_slot slot) {
        struct cancellation_reset final {
            asio::cancellation_slot slot_;
            ~cancellation_reset() {
                if (slot_.is_connected()) {
                    slot_.clear();
                }
            }
        } reset{slot};
        if (slot.is_connected()) {
            slot.assign([this](asio::cancellation_type) { response_.data_signal_.notify(); });
        }
        while (response_.offset_ == response_.buffered_.size() && response_.pending_.empty() &&
               !response_.receive_complete() && !response_.failure_ && !response_.pool_closed_ && !response_.abandoned_) {
            check();
            co_await response_.data_signal_.wait();
        }
        check();
        try {
            const auto chunk = co_await tunnel_.read();
            if (!chunk) {
                co_return false;
            }
            bytes.append(as_chars(*chunk));
            co_return true;
        } catch (const http_client_error& error) {
            throw inbound_transport_error(transport_error(error), "WebSocket tunnel read");
        }
    }
    task<std::error_code> write_bytes(std::string_view bytes, http_stream_end end) {
        try {
            while (!bytes.empty()) {
                const auto chunk = bytes.substr(0, 16384);
                co_await tunnel_.write(chunk);
                activity_.note_write_progress();
                bytes.remove_prefix(chunk.size());
            }
            if (end == http_stream_end::end) {
                co_await tunnel_.finish();
            }
            co_return std::error_code{};
        } catch (const http_client_error& error) {
            co_return transport_error(error);
        }
    }
    void abort() noexcept {
        tunnel_.abort();
        response_.data_signal_.notify();
    }
    http_client_response_state& response() noexcept {
        return response_;
    }

private:
    static std::error_code transport_error(const http_client_error& error) noexcept {
        if (error.code() == std::errc::timed_out) {
            return std::make_error_code(std::errc::timed_out);
        }
        if (error.code() == std::errc::operation_canceled) {
            return std::make_error_code(std::errc::operation_canceled);
        }
        return std::make_error_code(std::errc::io_error);
    }
    http_client_tunnel tunnel_;
    http_client_response_state& response_;
    connection_scanner::entry& activity_;
};

class client_websocket_session final : public std::enable_shared_from_this<client_websocket_session> {
public:
    client_websocket_session(http_client_pool& pool, http_client_tunnel tunnel,
        const http_client_websocket_handshake& handshake, websocket_client_deflate_offer deflate,
        websocket_lifecycle_options lifecycle, std::size_t message_limit)
        : worker_(pool.worker()),
          retire_signal_(worker_),
          scanner_(worker_, scanner_options(lifecycle)),
          connection_(http_client_websocket_transport(std::move(tunnel), entry_), worker_, entry_, normalize_websocket_lifecycle_options(lifecycle),
              protocol_byte_limit::limited(message_limit), *process_resource(), {},
              {.compression_ = handshake.compression_, .role_ = websocket_connection_role::client, .mask_key_generator_ = &mask_key, .deflater_ = deflate.deflater_},
              std::pmr::string(handshake.selected_subprotocol_, process_resource())),
          scanner_guard_(scanner_, entry_, {&connection_, &abort_transport, &abort_transport}) {
        entry_.enter_long_lived();
        scanner_.start();
    }
    static void retire(void* target) noexcept {
        auto& self = *static_cast<client_websocket_session*>(target);
        self.retiring_ = true;
        self.retire_signal_.notify();
    }
    static task<void> run(std::shared_ptr<client_websocket_session> self, stop_token pool_stop) {
        auto stop_registration = pool_stop.register_callback([self = self.get()] {
            self->retire_signal_.notify();
        });
        auto& response = self->connection_.transport().response();
        while (!self->retiring_ && !response.pool_closed_ && !pool_stop.stop_requested()) {
            co_await self->retire_signal_.wait();
        }
        stop_registration.reset();
        self->scanner_.stop();
        co_await self->connection_.detach_and_drain_writes();
        if (const auto failure = self->connection_.take_background_failure()) {
            report_unhandled_failure("WebSocket client session", failure);
        }
    }

private:
    static connection_scanner_options scanner_options(const websocket_lifecycle_options& lifecycle) {
        auto interval = std::chrono::milliseconds(1000);
        const auto tighten = [&](const std::optional<timeout_duration>& timeout) {
            if (timeout) {
                interval = std::min(interval, std::max(std::chrono::milliseconds(1), timeout->value() / 2));
            }
        };
        tighten(lifecycle.heartbeat_.ping_interval_);
        tighten(lifecycle.heartbeat_.pong_timeout_);
        tighten(lifecycle.close_handshake_timeout_);
        return {.scan_interval_ = interval};
    }
    static bool mask_key(void*, websocket_mask_key_type& key) noexcept {
        return fill_random_bytes(std::span<unsigned char>(reinterpret_cast<unsigned char*>(key.data()), key.size()));
    }
    static void abort_transport(void* target) noexcept {
        static_cast<websocket_session_connection<http_client_websocket_transport>*>(target)->abort();
    }
    worker_handle worker_;
    worker_signal retire_signal_;
    connection_scanner scanner_;
    connection_scanner::entry entry_;

public:
    websocket_session_connection<http_client_websocket_transport> connection_;

private:
    connection_scanner::guard scanner_guard_;
    bool retiring_{false};
};

}  // namespace ruvia::detail

namespace ruvia {

http_client_websocket_request::http_client_websocket_request(const http_client_handle& client, std::string_view path)
    : client_(client),
      path_(path, process_resource()),
      headers_(process_resource()),
      subprotocols_(process_resource()) {
    require_open();
    if (!is_valid_http_origin_form_target(path_)) {
        throw std::invalid_argument("WebSocket path must use origin-form");
    }
}
void http_client_websocket_request::require_open() const {
    client_.registration_.require_active();
    if (!client_.pool_->worker().is_current() || consumed_) {
        throw std::logic_error("WebSocket builder is consumed or used outside its worker");
    }
}
http_client_websocket_request& http_client_websocket_request::header(std::string_view name, std::string_view value) {
    require_open();
    headers_.emplace_back(name, value, headers_.get_allocator().resource());
    return *this;
}
http_client_websocket_request& http_client_websocket_request::headers(std::span<const http_header_view> fields) {
    require_open();
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
http_client_websocket_request& http_client_websocket_request::subprotocol(std::string_view value) {
    require_open();
    subprotocols_.emplace_back(value);
    return *this;
}
http_client_websocket_request& http_client_websocket_request::deflate(websocket_client_deflate_offer offer) {
    require_open();
    deflate_ = offer;
    return *this;
}
http_client_websocket_request& http_client_websocket_request::lifecycle(websocket_lifecycle_options options) {
    require_open();
    lifecycle_ = detail::normalize_websocket_lifecycle_options(options);
    return *this;
}
http_client_websocket_request& http_client_websocket_request::max_message_bytes(std::size_t limit) {
    require_open();
    if (limit == 0) {
        throw std::invalid_argument("WebSocket message limit must be greater than zero");
    }
    max_message_bytes_ = limit;
    return *this;
}
http_client_websocket_request& http_client_websocket_request::configure(http_client_request_options options) {
    require_open();
    if (options.field_source_ != http_request_field_source::generated &&
        options.field_source_ != http_request_field_source::forwarded) {
        throw std::invalid_argument("invalid HTTP request field source");
    }
    request_options_ = std::move(options);
    return *this;
}
task<websocket> http_client_websocket_request::connect() {
    require_open();
    consumed_ = true;
    return make_scoped_operation(client_.registration_.scope(), connect_owned(std::move(*this)));
}
task<websocket> http_client_websocket_request::connect_owned(http_client_websocket_request request) {
    request.client_.registration_.require_active();
    auto& pool = *request.client_.pool_;
    auto* resource = process_resource();
    std::shared_ptr<detail::http_client_websocket_handshake> handshake;
    std::optional<detail::http_client_request_storage> stored;
    {
        const detail::inline_http_header_views headers(request.headers_, resource);
        validate_http1_websocket_client_handshake_configuration(headers.view());
        std::pmr::vector<std::string_view> protocols(resource);
        protocols.reserve(request.subprotocols_.size());
        for (const auto& protocol : request.subprotocols_) {
            protocols.emplace_back(protocol);
        }
        handshake = std::allocate_shared<detail::http_client_websocket_handshake>(
            std::pmr::polymorphic_allocator<detail::http_client_websocket_handshake>(resource),
            websocket_client_negotiation_config_view{.subprotocols_ = std::span<const std::string_view>(protocols), .deflate_ = request.deflate_}, resource);
        stored.emplace("GET", request.path_, headers.view(), resource);
        stored->options_ = detail::copy_http_client_request_settings(request.request_options_);
        stored->informational_ = detail::share_http_client_informational(std::move(request.request_options_.informational_));
        stored->pool_owner_ = request.client_.pool_owner_;
        stored->set_host_stop(request.client_.host_stop_token_);
        stored->set_tunnel(pool.config().authority_, "websocket");
        stored->set_websocket(handshake);
    }
    std::pmr::string(request.path_.get_allocator()).swap(request.path_);
    std::pmr::vector<http_header>(request.headers_.get_allocator()).swap(request.headers_);
    std::pmr::vector<std::pmr::string>(request.subprotocols_.get_allocator()).swap(request.subprotocols_);
    // The opening deadline covers admission, transport establishment and every
    // handshake byte, but must not become the established tunnel's lifetime.
    // After opening, user cancellation belongs to each operation's snapshot.
    stop_source opening_stop;
    auto user_stop = request.client_.options_.stop_token_.register_callback(
        [&opening_stop] { opening_stop.request_stop(); });
    bool opening_expired = false;
    worker_timer_registration opening_timer;
    const auto connect_timeout = request.request_options_.timeouts_.connect_.resolve(pool.config().connect_timeout_);
    if (connect_timeout) {
        (void)pool.worker().schedule_timer(opening_timer, deadline_after(connect_timeout->value()),
            [&opening_stop, &opening_expired](worker_timer_outcome outcome) noexcept {
                if (outcome == worker_timer_outcome::expired) {
                    opening_expired = true;
                    opening_stop.request_stop();
                }
            });
    }
    auto opening_options = request.client_.options_;
    opening_options.stop_token_ = opening_stop.token();
    std::optional<http_client_tunnel_result> opened;
    try {
        opened.emplace(co_await pool.open_tunnel(std::move(*stored), {}, std::move(opening_options)));
    } catch (...) {
        opening_timer.cancel();
        if (opening_expired) {
            throw http_client_error(std::make_error_code(std::errc::timed_out), "WebSocket opening handshake timed out");
        }
        throw;
    }
    opening_timer.cancel();
    user_stop.reset();
    if (opening_expired) {
        throw http_client_error(std::make_error_code(std::errc::timed_out), "WebSocket opening handshake timed out");
    }
    auto* tunnel = opened->tunnel();
    if (!tunnel) {
        throw http_client_error(make_error_code(http_client_errc::handshake_rejected),
            "WebSocket handshake rejected", opened->response()->status());
    }
    auto owner = std::allocate_shared<detail::client_websocket_session>(
        std::pmr::polymorphic_allocator<detail::client_websocket_session>(resource),
        pool, std::move(*tunnel), *handshake, request.deflate_, request.lifecycle_, request.max_message_bytes_);
    auto result = detail::make_websocket_facade(owner->connection_, *resource);
    pool.tasks().spawn(detail::client_websocket_session::run(owner, pool.pool_stop_token()));
    result.state_->owner_ = owner;
    result.state_->retire_ = &detail::client_websocket_session::retire;
    result.state_->defaults_ = request.client_.options_;
    result.state_->read_timeout_ = request.request_options_.timeouts_.read_.resolve(pool.config().read_timeout_);
    co_return result;
}
http_client_websocket_request http_client_handle::websocket(std::string_view path) const {
    registration_.require_active();
    return http_client_websocket_request(*this, path);
}

}  // namespace ruvia
