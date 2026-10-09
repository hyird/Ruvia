#pragma once

#include <memory_resource>
#include <optional>
#include <span>
#include <utility>

#include "ruvia/core/stop_token.h"
#include "ruvia/web/streaming.h"

namespace ruvia::detail {

struct streaming_access final {
    using body_read_type = callable_ref<std::optional<std::span<const std::byte>>>::invoke_type;
    using stream_write_type = response_stream_writer::write_type;
    using stream_end_type = response_stream_writer::end_type;
    using stream_sleep_type = response_stream_writer::sleep_type;
    using stream_bind_context_type = response_stream_writer::bind_context_type;
    using stream_release_context_type = response_stream_writer::release_context_type;
    using stream_committed_type = response_stream_writer::committed_type;
    using stream_aborted_type = response_stream_writer::aborted_type;

    static void emplace_body_reader(std::optional<body_reader>& storage, void* target, body_read_type read) {
        storage.emplace(body_reader::token_type{}, target, read);
    }

    [[nodiscard]] static body_reader make_body_reader(void* target, body_read_type read) noexcept {
        return body_reader(body_reader::token_type{}, target, read);
    }

    [[nodiscard]] static response_stream_writer make_response_stream_writer(std::pmr::memory_resource& resource, void* target,
        stream_write_type write, stream_end_type end, stream_sleep_type sleep, stream_bind_context_type bind_context,
        stream_release_context_type release_context, stream_committed_type committed,
        stream_aborted_type aborted) noexcept {
        return response_stream_writer(
            resource, target, write, end, sleep, bind_context, release_context, committed, aborted);
    }

    [[nodiscard]] static sse_writer make_sse_writer(response_stream_writer& writer) noexcept {
        return sse_writer(writer);
    }

    // The token is supplied by the caller rather than read from `context`:
    // context is incomplete here, and this header is deliberately narrow.
    static void bind_context(response_stream_writer& writer, context& context_value, stop_token stop_token_value,
        response_stream_writer::streaming_head_thunk_type streaming_head) {
        writer.bind_context(context_value, std::move(stop_token_value), streaming_head);
    }

    static void release_context(response_stream_writer& writer) noexcept {
        writer.release_context();
    }

    [[nodiscard]] static bool committed(const response_stream_writer& writer) noexcept {
        return writer.committed();
    }
};

}  // namespace ruvia::detail
