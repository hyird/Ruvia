#pragma once

#include <cstddef>
#include <exception>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/core/task_scope.h"
#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http2_connection.h"

namespace ruvia::detail {
class websocket_client_state;

// One worker owns the connection and its sole socket reader/writer. Received
// DATA retains its linear credit until the application consumes the buffer.
class websocket_http2_transport final {
public:
    websocket_http2_transport(websocket_client_state& owner_value, const worker_handle& worker_value,
        std::pmr::memory_resource* resource);
    [[nodiscard]] task<void> connect();
    [[nodiscard]] task<std::size_t> read(std::span<char> output);
    [[nodiscard]] task<void> write(std::string_view bytes);
    [[nodiscard]] task<void> finish();
    void stop() noexcept;
    [[nodiscard]] task<void> join();

private:
    [[nodiscard]] task<void> run_reader();
    [[nodiscard]] task<void> run_writer();
    [[nodiscard]] task<void> flush();
    void drain_events();
    void check_failure() const;
    void fail(std::exception_ptr failure) noexcept;

    websocket_client_state& owner_;
    ruvia::http2_connection connection_;
    task_scope drivers_;
    worker_signal progress_;
    worker_signal writer_wake_;
    std::pmr::string received_;
    std::size_t read_offset_{0};
    std::optional<http2_received_data_credit> received_credit_{};
    std::optional<http_client_response_head> response_{};
    std::exception_ptr failure_{};
    std::uint32_t stream_id_{0};
    bool writer_active_{false};
    bool eof_{false};
    bool clean_reset_{false};
    bool stopped_{false};
};
}  // namespace ruvia::detail
