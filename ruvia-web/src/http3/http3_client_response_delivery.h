#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>

#include "ruvia/http/http_response.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_response_state.h"
#include "http3/http3_client_body_budget.h"
#include "http3/http3_client_receive_driver.h"

namespace ruvia::detail {

// Non-owning bridge from synchronous HTTP/3 parser events to one stable client
// response. The adapter and borrowed http_client_response_state must stay at fixed
// addresses on their owning worker until the QUIC stream can no longer deliver
// events. The callback copies all event views before returning and never writes
// to state.buffered, whose storage may back a view already returned to a reader.
//
// The connection owner must not retire HTTP/3 parser state from a callback.
// After drive() returns, it owns any required QUIC STOP_SENDING / stream
// termination, must guarantee that no more stream bytes can arrive, and only
// then may retire the parser and commit a terminal result here. Engine parser
// retirement alone does not stop QUIC delivery. The owner must join any driver
// that can use the adapter before destroying either borrowed object.
//
// read_allowance() bounds only bytes retained in pending + buffered. It does
// not bound allocation capacity, QUIC/OpenSSL buffering or QPACK parser state.
// collect_all_ uses buffered_limit_ as the producer's unread-body limit; the
// public read_all(max_bytes) keeps its separate final result-size check. At the
// producer limit, bounded wire probes still allow trailers and FIN to arrive.
class http3_client_response_delivery final {
public:
    enum class read_status_type : std::uint8_t {
        ready,
        backpressured,
        retirement_required,
        terminal,
    };

    struct read_allowance_type final {
        read_status_type status_{read_status_type::backpressured};
        std::size_t bytes_{};
    };

    enum class retirement_reason_type : std::uint8_t {
        none,
        response_too_large,
        protocol_error,
        callback_failure,
    };

    enum class commit_status_type : std::uint8_t {
        pending,
        committed,
        retirement_required,
        already_committed,
    };

    explicit http3_client_response_delivery(http_client_response_state& state_value,
        http3_client_body_budget* body_budget = nullptr)
        : state_(state_value) {
        if (body_budget != nullptr && !state_.bind_http3_body_budget(*body_budget)) {
            throw std::length_error("HTTP/3 response body budget is exhausted");
        }
    }
    http3_client_response_delivery(const http3_client_response_delivery&) = delete;
    http3_client_response_delivery& operator=(const http3_client_response_delivery&) = delete;
    http3_client_response_delivery(http3_client_response_delivery&&) = delete;
    http3_client_response_delivery& operator=(http3_client_response_delivery&&) = delete;

    [[nodiscard]] http3_client_response_event_sink event_sink() noexcept;
    [[nodiscard]] read_allowance_type read_allowance(std::size_t max_input_bytes) const noexcept;
    [[nodiscard]] retirement_reason_type retirement_reason() const noexcept {
        return retirement_reason_;
    }
    [[nodiscard]] std::exception_ptr callback_failure() const noexcept {
        return callback_failure_;
    }
    [[nodiscard]] std::size_t retained_body_bytes() const noexcept {
        return state_.producer_body_bytes();
    }
    void reconcile_body_bytes() noexcept;
    // Owns the protocol-computed final-head classification by value, so it
    // remains usable after the parser has been retired.
    [[nodiscard]] std::optional<http_response_body_plan> response_body_plan() const noexcept {
        return response_body_plan_;
    }

    // Call only after drive() returned and the owner completed any required
    // QUIC/parser retirement. Nonterminal driver results leave the response
    // pending. A pending retirement request must instead be committed with
    // commit_retirement_failure() after the stream is stopped and retired.
    [[nodiscard]] commit_status_type commit(const http3_client_receive_driver::result_type& result) noexcept;
    [[nodiscard]] commit_status_type commit_complete() noexcept;
    [[nodiscard]] bool commit_terminal_error(http_client_error::code_type error) noexcept;

    // The caller must first stop QUIC delivery and retire the HTTP/3 parser for
    // this stream. Use response_too_large for an ordinary retention overflow.
    [[nodiscard]] bool commit_retirement_failure(http_client_error::code_type error) noexcept;

    // Used after drive() has unwound (including a callback allocation failure)
    // and the outer owner has completed its connection/stream teardown.
    [[nodiscard]] bool commit_failure(std::exception_ptr failure) noexcept;

private:
    static void on_event(void* context, const http3_connection_event& event);
    void deliver(const http3_connection_event& event);
    void commit_error(http_client_error::code_type error) noexcept;
    void commit_terminal() noexcept;
    void request_retirement(retirement_reason_type reason) noexcept;
    [[nodiscard]] std::size_t remaining_body_capacity() const noexcept;

    http_client_response_state& state_;
    std::optional<http_response_body_plan> response_body_plan_{};
    std::exception_ptr callback_failure_{};
    retirement_reason_type retirement_reason_{retirement_reason_type::none};
};

}  // namespace ruvia::detail
