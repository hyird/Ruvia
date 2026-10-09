#include <concepts>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "http2/http2_stream_state.h"
#include "test_harness.h"

namespace {

using ruvia::detail::http2_local_connect_pending;
using ruvia::detail::http2_local_end_stream_committed;
using ruvia::detail::http2_local_end_stream_queued;
using ruvia::detail::http2_local_head_pending;
using ruvia::detail::http2_local_request_content_open;
using ruvia::detail::http2_local_response_content_open;
using ruvia::detail::http2_local_response_trailers_only;
using ruvia::detail::http2_local_send_state;
using ruvia::detail::http2_local_tunnel_open;
using ruvia::detail::http2_remote_aborted;
using ruvia::detail::http2_remote_connect_pending;
using ruvia::detail::http2_remote_connect_pending_end_stream;
using ruvia::detail::http2_remote_connect_rejected_awaiting_end_stream;
using ruvia::detail::http2_remote_content_open;
using ruvia::detail::http2_remote_end_stream;
using ruvia::detail::http2_remote_head_end_stream_pending;
using ruvia::detail::http2_remote_head_pending;
using ruvia::detail::http2_remote_receive_state;
using ruvia::detail::http2_remote_tunnel_open;
using ruvia::detail::http2_stream_aborted;
using ruvia::detail::http2_stream_close_source;
using ruvia::detail::http2_stream_lifecycle;
using ruvia::detail::http2_stream_state;

}  // namespace

RUVIA_TEST(http2_local_send_state_request_content_has_exclusive_transitions) {
    std::pmr::monotonic_buffer_resource resource;
    http2_stream_state stream(1, &resource);
    const auto& state_value = stream.local_send();
    RUVIA_CHECK(state_value.head_pending() != nullptr);
    RUVIA_CHECK(state_value.request_content_open() == nullptr);
    RUVIA_CHECK(state_value.response_content_open() == nullptr);
    RUVIA_CHECK(state_value.response_trailers_only() == nullptr);
    RUVIA_CHECK(state_value.connect_pending() == nullptr);
    RUVIA_CHECK(state_value.tunnel_open() == nullptr);
    RUVIA_CHECK(state_value.end_stream_queued() == nullptr);
    RUVIA_CHECK(state_value.end_stream_committed() == nullptr);
    RUVIA_CHECK(state_value.aborted() == nullptr);

    RUVIA_CHECK(stream.begin_local_request_content());
    RUVIA_CHECK(state_value.head_pending() == nullptr);
    RUVIA_CHECK(state_value.request_content_open() != nullptr);
    RUVIA_CHECK(!stream.begin_local_request_content());
    RUVIA_CHECK(!stream.begin_local_response_content());
    RUVIA_CHECK(!stream.commit_local_head_end_stream());

    RUVIA_CHECK(stream.queue_local_end_stream());
    RUVIA_CHECK(state_value.request_content_open() == nullptr);
    RUVIA_CHECK(state_value.end_stream_queued() != nullptr);
    RUVIA_CHECK(!stream.queue_local_end_stream());
    RUVIA_CHECK(stream.commit_local_end_stream());
    RUVIA_CHECK(state_value.end_stream_queued() == nullptr);
    RUVIA_CHECK(state_value.end_stream_committed() != nullptr);
    RUVIA_CHECK(!stream.commit_local_end_stream());
}

RUVIA_TEST(http2_local_send_state_response_content_and_trailers_are_distinct) {
    std::pmr::monotonic_buffer_resource resource;
    http2_stream_state content_stream(1, &resource);
    const auto& content = content_stream.local_send();
    RUVIA_CHECK(content_stream.begin_local_response_content());
    RUVIA_CHECK(content.response_content_open() != nullptr);
    RUVIA_CHECK(content.response_trailers_only() == nullptr);
    RUVIA_CHECK(content_stream.commit_local_end_stream());
    RUVIA_CHECK(content.end_stream_committed() != nullptr);

    http2_stream_state trailer_stream(3, &resource);
    const auto& trailers = trailer_stream.local_send();
    RUVIA_CHECK(trailer_stream.begin_local_response_trailers_only());
    RUVIA_CHECK(trailers.response_content_open() == nullptr);
    RUVIA_CHECK(trailers.response_trailers_only() != nullptr);
    RUVIA_CHECK(!trailer_stream.begin_local_response_content());
    RUVIA_CHECK(trailer_stream.commit_local_end_stream());
    RUVIA_CHECK(trailers.end_stream_committed() != nullptr);
}

RUVIA_TEST(http2_local_send_state_head_end_stream_never_opens_content) {
    std::pmr::monotonic_buffer_resource resource;
    http2_stream_state stream(1, &resource);
    const auto& state_value = stream.local_send();
    RUVIA_CHECK(stream.commit_local_head_end_stream());
    RUVIA_CHECK(state_value.head_pending() == nullptr);
    RUVIA_CHECK(state_value.request_content_open() == nullptr);
    RUVIA_CHECK(state_value.response_content_open() == nullptr);
    RUVIA_CHECK(state_value.end_stream_committed() != nullptr);
    RUVIA_CHECK(!stream.begin_local_request_content());
    RUVIA_CHECK(!stream.queue_local_end_stream());
}

RUVIA_TEST(http2_local_send_state_connect_waits_for_acceptance) {
    std::pmr::monotonic_buffer_resource resource;
    http2_stream_state accepted_stream(1, &resource);
    const auto& accepted = accepted_stream.local_send();
    RUVIA_CHECK(accepted_stream.begin_standard_connect());
    RUVIA_CHECK(accepted_stream.begin_local_connect_request());
    RUVIA_CHECK(accepted.connect_pending() != nullptr);
    RUVIA_CHECK(!accepted_stream.queue_local_end_stream());
    RUVIA_CHECK(!accepted_stream.commit_local_end_stream());
    RUVIA_CHECK(accepted_stream.accept_connect());
    RUVIA_CHECK(accepted_stream.open_local_connect_tunnel());
    RUVIA_CHECK(accepted.connect_pending() == nullptr);
    RUVIA_CHECK(accepted.tunnel_open() != nullptr);
    RUVIA_CHECK(accepted_stream.queue_local_end_stream());
    RUVIA_CHECK(accepted_stream.commit_local_end_stream());

    http2_stream_state rejected_stream(3, &resource);
    const auto& rejected = rejected_stream.local_send();
    RUVIA_CHECK(rejected_stream.begin_standard_connect());
    RUVIA_CHECK(rejected_stream.begin_local_connect_request());
    RUVIA_CHECK(rejected_stream.reject_connect());
    RUVIA_CHECK(rejected_stream.reject_local_connect());
    RUVIA_CHECK(rejected.end_stream_committed() != nullptr);
    RUVIA_CHECK(!rejected_stream.open_local_connect_tunnel());

    // A server sends the accepting response from its initial head-pending state;
    // the owning stream separately proves that this is a validated CONNECT.
    http2_stream_state server_stream(5, &resource);
    const auto& server = server_stream.local_send();
    RUVIA_CHECK(server_stream.begin_standard_connect());
    RUVIA_CHECK(server_stream.accept_connect());
    RUVIA_CHECK(server_stream.open_local_connect_tunnel());
    RUVIA_CHECK(server.tunnel_open() != nullptr);
}

RUVIA_TEST(http2_local_send_state_abort_owns_immutable_close_source) {
    std::pmr::monotonic_buffer_resource resource;
    http2_stream_state stream(1, &resource);
    const auto& state_value = stream.local_send();
    RUVIA_CHECK(!stream.abort(static_cast<http2_stream_close_source>(0xFF)));
    RUVIA_CHECK(state_value.head_pending() != nullptr);
    RUVIA_CHECK(stream.abort(http2_stream_close_source::local));
    const auto* aborted = state_value.aborted();
    RUVIA_CHECK(aborted != nullptr);
    RUVIA_CHECK(aborted != nullptr && aborted->source() == http2_stream_close_source::local);
    RUVIA_CHECK(!stream.abort(http2_stream_close_source::peer));
    RUVIA_CHECK(
        state_value.aborted() != nullptr && state_value.aborted()->source() == http2_stream_close_source::local);
    RUVIA_CHECK(!stream.begin_local_request_content());
    RUVIA_CHECK(!stream.commit_local_end_stream());
}

RUVIA_TEST(http2_remote_receive_state_owns_head_content_connect_and_terminal_transitions) {
    std::pmr::monotonic_buffer_resource resource;

    http2_stream_state content_stream(1, &resource);
    const auto& content = content_stream.remote_receive();
    RUVIA_CHECK(content.head_pending() != nullptr);
    RUVIA_CHECK(content_stream.finalize_remote_content_head());
    RUVIA_CHECK(content.content_open() != nullptr);
    RUVIA_CHECK(content_stream.finish_remote_content());
    RUVIA_CHECK(content.end_stream() != nullptr);
    RUVIA_CHECK(!content_stream.finish_remote_content());

    http2_stream_state head_ended(3, &resource);
    const auto& ended = head_ended.remote_receive();
    RUVIA_CHECK(head_ended.record_remote_head_end_stream());
    RUVIA_CHECK(ended.head_end_stream_pending() != nullptr);
    RUVIA_CHECK(head_ended.finalize_remote_content_head());
    RUVIA_CHECK(ended.end_stream() != nullptr);

    http2_stream_state rejected_connect(5, &resource);
    const auto& rejected = rejected_connect.remote_receive();
    RUVIA_CHECK(rejected_connect.begin_standard_connect());
    RUVIA_CHECK(rejected_connect.finalize_remote_connect_head());
    RUVIA_CHECK(rejected.connect_pending() != nullptr);
    RUVIA_CHECK(rejected_connect.reject_connect());
    RUVIA_CHECK(rejected.connect_rejected_awaiting_end_stream() != nullptr);
    RUVIA_CHECK(rejected_connect.finish_remote_rejected_connect());
    RUVIA_CHECK(rejected.end_stream() != nullptr);

    http2_stream_state open_tunnel(7, &resource);
    const auto& tunnel = open_tunnel.remote_receive();
    RUVIA_CHECK(open_tunnel.begin_extended_connect());
    RUVIA_CHECK(open_tunnel.finalize_remote_connect_head());
    RUVIA_CHECK(open_tunnel.accept_connect());
    RUVIA_CHECK(tunnel.tunnel_open() != nullptr);
    RUVIA_CHECK(open_tunnel.finish_remote_tunnel());
    RUVIA_CHECK(tunnel.end_stream() != nullptr);

    http2_stream_state half_closed_connect(9, &resource);
    const auto& half_closed = half_closed_connect.remote_receive();
    RUVIA_CHECK(half_closed_connect.begin_standard_connect());
    RUVIA_CHECK(half_closed_connect.record_remote_head_end_stream());
    RUVIA_CHECK(half_closed_connect.finalize_remote_connect_head());
    RUVIA_CHECK(half_closed.connect_pending_end_stream() != nullptr);
    RUVIA_CHECK(half_closed_connect.accept_connect());
    RUVIA_CHECK(half_closed.end_stream() != nullptr);
}

RUVIA_TEST(stream_lifecycle_abort_sets_all_terminal_state) {
    std::pmr::monotonic_buffer_resource resource;
    http2_stream_state stream(1, &resource);
    RUVIA_CHECK(!stream.is_aborted());
    RUVIA_CHECK(stream.remote_receive().head_pending() != nullptr);
    RUVIA_CHECK(stream.local_send().head_pending() != nullptr);
    RUVIA_CHECK(!stream.abort(static_cast<http2_stream_close_source>(0xFF)));
    RUVIA_CHECK(stream.local_send().head_pending() != nullptr);

    RUVIA_CHECK(stream.abort(http2_stream_close_source::peer));
    RUVIA_CHECK(stream.is_aborted());
    RUVIA_CHECK(stream.remote_receive().aborted() != nullptr);
    const auto* aborted = stream.local_send().aborted();
    RUVIA_CHECK(aborted != nullptr);
    RUVIA_CHECK(aborted != nullptr && aborted->source() == http2_stream_close_source::peer);
    RUVIA_CHECK(!stream.abort(http2_stream_close_source::peer_goaway));
    RUVIA_CHECK(stream.local_send().aborted()->source() == http2_stream_close_source::peer);
}

RUVIA_TEST(stream_lifecycle_abort_blocks_queue_and_dispatch) {
    std::pmr::monotonic_buffer_resource resource;
    http2_stream_state lifecycle(1, &resource);
    RUVIA_CHECK(lifecycle.try_mark_queued());
    RUVIA_CHECK(lifecycle.queued());
    RUVIA_CHECK(!lifecycle.try_mark_queued());
    lifecycle.clear_queued();
    RUVIA_CHECK(!lifecycle.queued());
    RUVIA_CHECK(lifecycle.try_mark_queued());
    RUVIA_CHECK(lifecycle.try_start_dispatch());
    RUVIA_CHECK(!lifecycle.queued());
    RUVIA_CHECK(lifecycle.dispatch_started());
    RUVIA_CHECK(!lifecycle.try_start_dispatch());

    http2_stream_state aborted_first(3, &resource);
    RUVIA_CHECK(aborted_first.abort(http2_stream_close_source::local));
    RUVIA_CHECK(aborted_first.remote_receive().aborted() != nullptr);
    RUVIA_CHECK(!aborted_first.try_mark_queued());
    RUVIA_CHECK(!aborted_first.try_start_dispatch());

    http2_stream_state queued_then_aborted(5, &resource);
    RUVIA_CHECK(queued_then_aborted.try_mark_queued());
    RUVIA_CHECK(queued_then_aborted.abort(http2_stream_close_source::peer));
    RUVIA_CHECK(!queued_then_aborted.queued());
    RUVIA_CHECK(queued_then_aborted.remote_receive().aborted() != nullptr);
    RUVIA_CHECK(!queued_then_aborted.try_start_dispatch());
}
