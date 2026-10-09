#pragma once

#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/core/task.h"
#include "ruvia/web/http_client_response.h"

#include "client/http_client_response_state.h"

namespace ruvia::detail {

struct http_client_output_write_input final {
    http_client_response pin_;
    std::pmr::string bytes_;
};

// The input pin and owned payload live in this single cold frame. Policies
// handle upload validation/trailers and tunnel retirement without another task.
template <typename policy>
[[nodiscard]] task<void> write_client_output(http_client_response_state* state_value, http_client_output_write_input input) {
    auto& output = policy::output(*state_value);
    policy::require_write(*state_value);
    if (input.bytes_.empty()) {
        co_return;
    }
    if constexpr (policy::validate_chunk) {
        policy::require_empty_chunk(output);
    }
    output.chunk_ = std::move(input.bytes_);
    output.chunk_ready_ = true;
    output.notify_data();
    while (output.chunk_ready_ && !output.stopped_) {
        co_await output.space_.wait();
    }
    if (output.stopped_) {
        policy::throw_stopped(*state_value);
    }
}

template <typename policy, typename input_type>
[[nodiscard]] task<void> finish_client_output(http_client_response_state* state_value, input_type input) {
    auto& output = policy::output(*state_value);
    if (!policy::begin_finish(*state_value, input)) {
        co_return;
    }
    output.end_requested_ = true;
    output.notify_data();
    while (!output.ended_ && (!output.stopped_ || output.completion_pending_)) {
        co_await output.space_.wait();
    }
    if (!output.ended_) {
        policy::throw_stopped(*state_value);
    }
}

}  // namespace ruvia::detail
