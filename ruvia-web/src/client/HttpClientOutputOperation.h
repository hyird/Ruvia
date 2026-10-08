#pragma once

#include <memory_resource>
#include <string>
#include <utility>

#include "ruvia/core/Task.h"
#include "ruvia/web/HttpClientResponse.h"

#include "client/HttpClientResponseState.h"

namespace ruvia::detail {

struct http_client_output_write_input final {
    HttpClientResponse pin;
    std::pmr::string bytes;
};

// The input pin and owned payload live in this single cold frame. Policies
// handle upload validation/trailers and tunnel retirement without another task.
template <typename policy>
[[nodiscard]] Task<void> write_client_output(HttpClientResponseState* state, http_client_output_write_input input) {
    auto& output = policy::output(*state);
    policy::require_write(*state);
    if (input.bytes.empty()) {
        co_return;
    }
    if constexpr (policy::validate_chunk) {
        policy::require_empty_chunk(output);
    }
    output.chunk = std::move(input.bytes);
    output.chunkReady = true;
    output.notifyData();
    while (output.chunkReady && !output.stopped) {
        co_await output.space.wait();
    }
    if (output.stopped) {
        policy::throw_stopped(*state);
    }
}

template <typename policy, typename input_type>
[[nodiscard]] Task<void> finish_client_output(HttpClientResponseState* state, input_type input) {
    auto& output = policy::output(*state);
    if (!policy::begin_finish(*state, input)) {
        co_return;
    }
    output.endRequested = true;
    output.notifyData();
    while (!output.ended && (!output.stopped || output.completionPending)) {
        co_await output.space.wait();
    }
    if (!output.ended) {
        policy::throw_stopped(*state);
    }
}

}  // namespace ruvia::detail
