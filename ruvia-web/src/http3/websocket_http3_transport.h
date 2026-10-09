#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http3_connection.h"

#include "http3/http3_quic_client_endpoint_resolver.h"
#include "http3/http3_quic_client_socket_session.h"

namespace ruvia::detail {
class websocket_client_state;

// A single worker-affine QUIC pump owns DNS, critical streams and one Extended
// CONNECT stream. Application operations publish bounded owned blocks and wake
// that pump. SSL WANT retries retain the original block until accepted or close.
class websocket_http3_transport final {
public:
    websocket_http3_transport(websocket_client_state& owner_value, const worker_handle& worker_value,
        std::pmr::memory_resource* resource);
    [[nodiscard]] task<void> connect();
    [[nodiscard]] task<std::size_t> read(std::span<char> output);
    [[nodiscard]] task<void> write(std::string_view bytes);
    [[nodiscard]] task<void> finish();
    void stop() noexcept;
    [[nodiscard]] task<void> join();

private:
    [[nodiscard]] task<void> drive();
    [[nodiscard]] task<void> wait_for_output();
    [[nodiscard]] bool receive(std::uint64_t id, bool request);
    [[nodiscard]] bool drive_output();
    void check_failure() const;
    void wake() noexcept;
    void prepare_frame(std::uint64_t type, std::span<const char> payload);
    static void on_event(void* context, const http3_connection_event& event);

    websocket_client_state& owner_;
    std::pmr::memory_resource* resource_;
    http3_quic_client_tls_context tls_;
    http3_quic_client_endpoint_resolver resolver_;
    ruvia::http3_connection connection_;
    task_scope drivers_;
    worker_signal progress_;
    std::optional<http3_quic_client_socket_session> session_{};
    std::pmr::vector<std::uint64_t> peer_streams_;
    std::pmr::string received_;
    std::size_t read_offset_{0};
    std::pmr::string outbound_;
    std::size_t write_offset_{0};
    std::pmr::string blocked_input_;
    std::array<std::pmr::string, 2> critical_output_;
    std::array<std::size_t, 2> critical_offset_{};
    std::array<worker_timer_registration, 2> critical_timers_{};
    std::optional<http3_message_head> response_{};
    std::exception_ptr failure_{};
    std::optional<std::uint64_t> stream_id_{};
    bool open_requested_{false};
    bool finish_requested_{false};
    bool finished_{false};
    bool fin_prepared_{false};
    bool qpack_blocked_{false};
    bool blocked_fin_{false};
    bool eof_{false};
    bool stopped_{false};
};
}  // namespace ruvia::detail
