#pragma once

#include <memory_resource>
#include <utility>

#include "ruvia/core/stop_token.h"
#include "ruvia/web/streaming.h"

namespace ruvia::detail {

struct streaming_access final {
    [[nodiscard]] static body_reader make_body_reader(void* target, body_reader::read_type::invoke_type read) noexcept {
        return body_reader(target, read);
    }

    [[nodiscard]] static response_stream_writer make_response_stream_writer(std::pmr::memory_resource& resource,
        void* target, response_stream_writer::commit_type commit, response_stream_writer::write_type write,
        response_stream_writer::end_type end, response_stream_writer::sleep_type sleep,
        response_stream_writer::bind_context_type bind_context,
        response_stream_writer::release_context_type release_context,
        response_stream_writer::committed_type committed, response_stream_writer::aborted_type aborted) noexcept {
        return response_stream_writer(
            resource, target, commit, write, end, sleep, bind_context, release_context, committed, aborted);
    }
    static void bind_worker(body_reader& reader, const worker_handle& worker_value) noexcept {
        reader.bind_worker(worker_value);
    }

    static void bind_worker(response_stream_writer& writer, const worker_handle& worker_value) noexcept {
        writer.bind_worker(worker_value);
    }

    // The token is supplied by the caller rather than read from `context`:
    // context is incomplete here, and this header is deliberately narrow.
    static void bind_context(response_stream_writer& writer, context& context_value, stop_token stop_token_value) {
        writer.bind_context(context_value, std::move(stop_token_value));
    }

    static void release_context(response_stream_writer& writer) noexcept {
        writer.release_context();
    }
};

}  // namespace ruvia::detail
