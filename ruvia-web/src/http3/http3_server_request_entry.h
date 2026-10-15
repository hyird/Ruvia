#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/worker_signal.h"
#include "ruvia/http/http_response.h"

#include "http3/http3_buffered_request_dispatch.h"
#include "http3/http3_buffered_response_output.h"
#include "http3/http3_server_connection.h"
#include "server/connection_scanner.h"
#include "server/http_buffered_response.h"

// Per-stream records of http3_server_connection, shared by its translation
// units (request lifecycle, publication, push and tunnels).

namespace ruvia::detail {

struct http3_server_connection::request_entry_type final {
    request_entry_type(http3_server_connection& owner_value, request_index_slot_type& slot_value,
        std::uint64_t stream_id_value)
        : owner_scanner_(owner_value.connection_scanner_),
          slot_(slot_value),
          stream_id_(stream_id_value),
          dispatch_(owner_value, stream_id_value, slot_value.push_id_,
              owner_value.input_.received_early_data(stream_id_value), scanner_entry_),
          publication_finished_(owner_value.services_.worker()) {}

    ~request_entry_type() = default;

    request_entry_type(const request_entry_type&) = delete;
    request_entry_type& operator=(const request_entry_type&) = delete;

    connection_scanner::entry_type scanner_entry_;
    connection_scanner* owner_scanner_{};

    request_index_slot_type& slot_;
    const std::uint64_t stream_id_;
    dispatch_type dispatch_;
    worker_signal publication_finished_;
    std::optional<connection_scanner::guard_type> scanner_registration_;
    bool output_enabled_{true};
    // Publication can terminate while the request frame still awaits peer FIN.
    bool output_terminal_{};
    // Independent permission for run_request to retire after cancellation.
    bool retirement_granted_{};
    bool cancel_requested_{};
};

// A protocol-level rejection (413/417/501/503) answered before any handler.
struct http3_server_connection::rejection_entry_type final {
    rejection_entry_type(http3_server_connection& owner_value, request_index_slot_type& slot_value,
        std::uint64_t stream_id_value, http_known_method method, http_status_code status)
        : slot_(slot_value),
          stream_id_(stream_id_value),
          response_(std::in_place, http_response::options_type{.resource_ = owner_value.worker_.resource()}) {
        response_->status(status);
        response_->header("content-length", "0");
        const auto write_plan = plan_buffered_http_response_write(method, *response_);
        auto created = http3_buffered_response_output::create(
            *response_, write_plan, owner_value.worker_, owner_value.output(), stream_id_);
        if (created.index() != 0) {
            throw std::runtime_error("HTTP/3 rejection response preparation failed");
        }
        output_.emplace(std::move(std::get<0>(created)));
    }

    rejection_entry_type(const rejection_entry_type&) = delete;
    rejection_entry_type& operator=(const rejection_entry_type&) = delete;

    request_index_slot_type& slot_;
    const std::uint64_t stream_id_;
    std::optional<http_response> response_;
    std::optional<http3_buffered_response_output> output_;
    bool output_enabled_{true};
    bool output_terminal_{};
    bool receive_terminal_{};
};

}  // namespace ruvia::detail
