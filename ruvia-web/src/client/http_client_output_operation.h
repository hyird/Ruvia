#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/web/http_client.h"

#include "client/http_client_pool.h"
#include "client/http_client_response_state.h"
#include "util/operation_lane_lease.h"

namespace ruvia::detail {

struct http_client_output_write_input final {
    http_client_response pin_;
    std::pmr::string bytes_;
};

struct in_flight_output_guard final {
    http_client_response_state& state_;
    http_client_output_queue& output_;
    bool active_{true};
    ~in_flight_output_guard() noexcept {
        if (!active_) {
            return;
        }
        output_.stop();
        if (state_.pool_ != nullptr && !state_.complete_ && !state_.abandoned_) {
            state_.pool_->abandon_response(state_);
        }
    }
};

// The input pin and owned payload live in this single cold frame. Policies
// handle upload validation/trailers and tunnel retirement without another task.
template <typename policy>
[[nodiscard]] task<void> write_client_output(http_client_response_state* state_value, http_client_output_write_input input) {
    auto& output = policy::output(*state_value);
    auto activity = claim_operation_lane(output.operation_active_, "HTTP client output operation is already active");
    policy::require_write(*state_value);
    if (output.chunk_ready_) {
        throw std::logic_error("HTTP client output queue still owns a chunk");
    }
    if (input.bytes_.empty()) {
        co_return;
    }
    output.chunk_ = std::move(input.bytes_);
    output.chunk_ready_ = true;
    in_flight_output_guard in_flight{*state_value, output};
    output.notify_data();
    while (output.chunk_ready_ && !output.stopped_) {
        co_await output.space_.wait();
    }
    in_flight.active_ = false;
    // Retirement may stop the queue after its driver acknowledged this chunk
    // but before this waiter resumes. It cannot revoke the completed write.
    if (output.chunk_ready_) {
        policy::throw_stopped(*state_value);
    }
}

template <typename policy, typename input_type>
[[nodiscard]] task<void> finish_client_output(http_client_response_state* state_value, input_type input) {
    auto& output = policy::output(*state_value);
    auto activity = claim_operation_lane(output.operation_active_, "HTTP client output operation is already active");
    if (!policy::begin_finish(*state_value, input)) {
        co_return;
    }
    if (output.chunk_ready_) {
        throw std::logic_error("HTTP client output queue still owns a chunk");
    }
    output.end_requested_ = true;
    in_flight_output_guard in_flight{*state_value, output};
    output.notify_data();
    while (!output.ended_ && (!output.stopped_ || output.completion_pending_)) {
        co_await output.space_.wait();
    }
    in_flight.active_ = false;
    if (!output.ended_) {
        policy::throw_stopped(*state_value);
    }
}

}  // namespace ruvia::detail
