#pragma once

#include <array>
#include <stdexcept>
#include <system_error>

#include <asio.hpp>

#include "ruvia/core/async.h"
#include "ruvia/core/task.h"
#include "ruvia/http/http1_interim_response_writer.h"

namespace ruvia::detail {

template <typename stream_type>
task<void> write_http1_continue(stream_type& stream) {
    std::array<char, 32> head_buffer{};
    const http_interim_response_head response(http_status::continue_value);
    const auto result_value = http1_interim_response_writer().prepare(response, head_buffer);
    const auto* const prepared = result_value.prepared();
    if (prepared == nullptr) {
        throw std::logic_error("failed to prepare HTTP/1 interim Continue response");
    }

    const auto write_completion =
        co_await ruvia::async_asio([&stream, head = prepared->head()](auto handler) mutable {
            asio::async_write(stream, asio::buffer(head), std::move(handler));
        });
    const auto ec = write_completion.error_code();
    if (ec) {
        throw std::system_error(ec, "failed to write HTTP/1 interim Continue response");
    }
}

}  // namespace ruvia::detail
