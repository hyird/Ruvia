#include "test_harness.h"

#include <concepts>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "ruvia/http/detail/http2/Http2StreamState.h"

namespace {

using ruvia::detail::Http2LocalConnectPending;
using ruvia::detail::Http2LocalEndStreamCommitted;
using ruvia::detail::Http2LocalEndStreamQueued;
using ruvia::detail::Http2LocalHeadPending;
using ruvia::detail::Http2LocalRequestContentOpen;
using ruvia::detail::Http2LocalResponseContentOpen;
using ruvia::detail::Http2LocalResponseTrailersOnly;
using ruvia::detail::Http2LocalSendState;
using ruvia::detail::Http2LocalTunnelOpen;
using ruvia::detail::Http2RemoteAborted;
using ruvia::detail::Http2RemoteConnectPending;
using ruvia::detail::Http2RemoteConnectPendingEndStream;
using ruvia::detail::Http2RemoteConnectRejectedAwaitingEndStream;
using ruvia::detail::Http2RemoteContentOpen;
using ruvia::detail::Http2RemoteEndStream;
using ruvia::detail::Http2RemoteHeadEndStreamPending;
using ruvia::detail::Http2RemoteHeadPending;
using ruvia::detail::Http2RemoteReceiveState;
using ruvia::detail::Http2RemoteTunnelOpen;
using ruvia::detail::Http2StreamAborted;
using ruvia::detail::Http2StreamCloseSource;
using ruvia::detail::Http2StreamLifecycle;
using ruvia::detail::Http2StreamState;

template <typename T>
concept HasCloseSource = requires(const T& value) {
    { value.source() } -> std::same_as<Http2StreamCloseSource>;
};

template <typename T>
concept HasStaleLocalSendProduct = requires(const T& value) {
    value.localSendPhase();
    value.localMessageKind();
    value.localEndStream();
    value.localEndStreamCommitted();
    value.localBodyOpen();
    value.localTrailersOnly();
    value.canSubmitLocalHead();
};

template <typename T>
concept HasStaleResetAccessor = requires(const T& value) {
    value.reset();
};

template <typename T>
concept HasStaleMarkReset = requires(T& value) {
    value.markReset(Http2StreamCloseSource::kLocal);
};

template <typename T>
concept HasStaleMarkClosed = requires(T& value) {
    value.markClosed(Http2StreamCloseSource::kLocal);
};

template <typename T>
concept HasStaleBodyEnded = requires(const T& value) {
    value.bodyEnded();
};

template <typename T>
concept HasStalePeerEndStream = requires(const T& value) {
    value.peerEndStream();
};

template <typename T>
concept HasStaleHeadersDecoded = requires(const T& value) {
    value.headersDecoded();
};

static_assert(!std::default_initializable<Http2LocalSendState>);
static_assert(!std::default_initializable<Http2LocalHeadPending>);
static_assert(!std::default_initializable<Http2LocalRequestContentOpen>);
static_assert(!std::default_initializable<Http2LocalResponseContentOpen>);
static_assert(!std::default_initializable<Http2LocalResponseTrailersOnly>);
static_assert(!std::default_initializable<Http2LocalConnectPending>);
static_assert(!std::default_initializable<Http2LocalTunnelOpen>);
static_assert(!std::default_initializable<Http2LocalEndStreamQueued>);
static_assert(!std::default_initializable<Http2LocalEndStreamCommitted>);
static_assert(!std::default_initializable<Http2StreamAborted>);
static_assert(!std::default_initializable<Http2RemoteReceiveState>);
static_assert(!std::default_initializable<Http2RemoteHeadPending>);
static_assert(!std::default_initializable<Http2RemoteHeadEndStreamPending>);
static_assert(!std::default_initializable<Http2RemoteContentOpen>);
static_assert(!std::default_initializable<Http2RemoteConnectPending>);
static_assert(!std::default_initializable<Http2RemoteConnectPendingEndStream>);
static_assert(!std::default_initializable<
    Http2RemoteConnectRejectedAwaitingEndStream>);
static_assert(!std::default_initializable<Http2RemoteTunnelOpen>);
static_assert(!std::default_initializable<Http2RemoteEndStream>);
static_assert(!std::default_initializable<Http2RemoteAborted>);
static_assert(!std::constructible_from<
    Http2StreamAborted,
    Http2StreamCloseSource>);
static_assert(!HasCloseSource<Http2LocalSendState>);
static_assert(!HasCloseSource<Http2LocalHeadPending>);
static_assert(!HasCloseSource<Http2LocalEndStreamCommitted>);
static_assert(HasCloseSource<Http2StreamAborted>);
static_assert(!HasStaleLocalSendProduct<Http2StreamLifecycle>);
static_assert(!std::default_initializable<Http2StreamLifecycle>);
static_assert(!HasStaleResetAccessor<Http2LocalSendState>);
static_assert(!HasStaleResetAccessor<Http2StreamLifecycle>);
static_assert(!HasStaleMarkReset<Http2StreamLifecycle>);
static_assert(!HasStaleMarkClosed<Http2StreamLifecycle>);
static_assert(std::same_as<
    decltype(std::declval<const Http2StreamLifecycle&>().localSend()),
    const Http2LocalSendState&>);
static_assert(std::same_as<
    decltype(std::declval<const Http2StreamLifecycle&>().remoteReceive()),
    const Http2RemoteReceiveState&>);
static_assert(!HasStaleBodyEnded<Http2StreamLifecycle>);
static_assert(!HasStalePeerEndStream<Http2StreamLifecycle>);
static_assert(!HasStaleBodyEnded<Http2StreamState>);
static_assert(!HasStalePeerEndStream<Http2StreamState>);
static_assert(!HasStaleHeadersDecoded<Http2StreamState>);

template <typename T>
concept ExposesRvalueHttp2StreamLifecycleStorage =
    requires(T&& lifecycle) { std::move(lifecycle).localSend(); } ||
    requires(T&& lifecycle) { std::move(lifecycle).remoteReceive(); };

static_assert(!ExposesRvalueHttp2StreamLifecycleStorage<
    Http2StreamLifecycle>);

}  // namespace

RUVIA_TEST(http2_local_send_state_request_content_has_exclusive_transitions) {
    std::pmr::monotonic_buffer_resource resource;
    Http2StreamState stream(1, &resource);
    const auto& state = stream.localSend();
    RUVIA_CHECK(state.headPending() != nullptr);
    RUVIA_CHECK(state.requestContentOpen() == nullptr);
    RUVIA_CHECK(state.responseContentOpen() == nullptr);
    RUVIA_CHECK(state.responseTrailersOnly() == nullptr);
    RUVIA_CHECK(state.connectPending() == nullptr);
    RUVIA_CHECK(state.tunnelOpen() == nullptr);
    RUVIA_CHECK(state.endStreamQueued() == nullptr);
    RUVIA_CHECK(state.endStreamCommitted() == nullptr);
    RUVIA_CHECK(state.aborted() == nullptr);

    RUVIA_CHECK(stream.beginLocalRequestContent());
    RUVIA_CHECK(state.headPending() == nullptr);
    RUVIA_CHECK(state.requestContentOpen() != nullptr);
    RUVIA_CHECK(!stream.beginLocalRequestContent());
    RUVIA_CHECK(!stream.beginLocalResponseContent());
    RUVIA_CHECK(!stream.commitLocalHeadEndStream());

    RUVIA_CHECK(stream.queueLocalEndStream());
    RUVIA_CHECK(state.requestContentOpen() == nullptr);
    RUVIA_CHECK(state.endStreamQueued() != nullptr);
    RUVIA_CHECK(!stream.queueLocalEndStream());
    RUVIA_CHECK(stream.commitLocalEndStream());
    RUVIA_CHECK(state.endStreamQueued() == nullptr);
    RUVIA_CHECK(state.endStreamCommitted() != nullptr);
    RUVIA_CHECK(!stream.commitLocalEndStream());
}

RUVIA_TEST(http2_local_send_state_response_content_and_trailers_are_distinct) {
    std::pmr::monotonic_buffer_resource resource;
    Http2StreamState contentStream(1, &resource);
    const auto& content = contentStream.localSend();
    RUVIA_CHECK(contentStream.beginLocalResponseContent());
    RUVIA_CHECK(content.responseContentOpen() != nullptr);
    RUVIA_CHECK(content.responseTrailersOnly() == nullptr);
    RUVIA_CHECK(contentStream.commitLocalEndStream());
    RUVIA_CHECK(content.endStreamCommitted() != nullptr);

    Http2StreamState trailerStream(3, &resource);
    const auto& trailers = trailerStream.localSend();
    RUVIA_CHECK(trailerStream.beginLocalResponseTrailersOnly());
    RUVIA_CHECK(trailers.responseContentOpen() == nullptr);
    RUVIA_CHECK(trailers.responseTrailersOnly() != nullptr);
    RUVIA_CHECK(!trailerStream.beginLocalResponseContent());
    RUVIA_CHECK(trailerStream.commitLocalEndStream());
    RUVIA_CHECK(trailers.endStreamCommitted() != nullptr);
}

RUVIA_TEST(http2_local_send_state_head_end_stream_never_opens_content) {
    std::pmr::monotonic_buffer_resource resource;
    Http2StreamState stream(1, &resource);
    const auto& state = stream.localSend();
    RUVIA_CHECK(stream.commitLocalHeadEndStream());
    RUVIA_CHECK(state.headPending() == nullptr);
    RUVIA_CHECK(state.requestContentOpen() == nullptr);
    RUVIA_CHECK(state.responseContentOpen() == nullptr);
    RUVIA_CHECK(state.endStreamCommitted() != nullptr);
    RUVIA_CHECK(!stream.beginLocalRequestContent());
    RUVIA_CHECK(!stream.queueLocalEndStream());
}

RUVIA_TEST(http2_local_send_state_connect_waits_for_acceptance) {
    std::pmr::monotonic_buffer_resource resource;
    Http2StreamState acceptedStream(1, &resource);
    const auto& accepted = acceptedStream.localSend();
    RUVIA_CHECK(acceptedStream.beginStandardConnect());
    RUVIA_CHECK(acceptedStream.beginLocalConnectRequest());
    RUVIA_CHECK(accepted.connectPending() != nullptr);
    RUVIA_CHECK(!acceptedStream.queueLocalEndStream());
    RUVIA_CHECK(!acceptedStream.commitLocalEndStream());
    RUVIA_CHECK(acceptedStream.acceptConnect());
    RUVIA_CHECK(acceptedStream.openLocalConnectTunnel());
    RUVIA_CHECK(accepted.connectPending() == nullptr);
    RUVIA_CHECK(accepted.tunnelOpen() != nullptr);
    RUVIA_CHECK(acceptedStream.queueLocalEndStream());
    RUVIA_CHECK(acceptedStream.commitLocalEndStream());

    Http2StreamState rejectedStream(3, &resource);
    const auto& rejected = rejectedStream.localSend();
    RUVIA_CHECK(rejectedStream.beginStandardConnect());
    RUVIA_CHECK(rejectedStream.beginLocalConnectRequest());
    RUVIA_CHECK(rejectedStream.rejectConnect());
    RUVIA_CHECK(rejectedStream.rejectLocalConnect());
    RUVIA_CHECK(rejected.endStreamCommitted() != nullptr);
    RUVIA_CHECK(!rejectedStream.openLocalConnectTunnel());

    // A server sends the accepting response from its initial head-pending state;
    // the owning stream separately proves that this is a validated CONNECT.
    Http2StreamState serverStream(5, &resource);
    const auto& server = serverStream.localSend();
    RUVIA_CHECK(serverStream.beginStandardConnect());
    RUVIA_CHECK(serverStream.acceptConnect());
    RUVIA_CHECK(serverStream.openLocalConnectTunnel());
    RUVIA_CHECK(server.tunnelOpen() != nullptr);
}

RUVIA_TEST(http2_local_send_state_abort_owns_immutable_close_source) {
    std::pmr::monotonic_buffer_resource resource;
    Http2StreamState stream(1, &resource);
    const auto& state = stream.localSend();
    RUVIA_CHECK(!stream.abort(static_cast<Http2StreamCloseSource>(0xFF)));
    RUVIA_CHECK(state.headPending() != nullptr);
    RUVIA_CHECK(stream.abort(Http2StreamCloseSource::kLocal));
    const auto* aborted = state.aborted();
    RUVIA_CHECK(aborted != nullptr);
    RUVIA_CHECK(aborted != nullptr &&
        aborted->source() == Http2StreamCloseSource::kLocal);
    RUVIA_CHECK(!stream.abort(Http2StreamCloseSource::kPeer));
    RUVIA_CHECK(state.aborted() != nullptr &&
        state.aborted()->source() == Http2StreamCloseSource::kLocal);
    RUVIA_CHECK(!stream.beginLocalRequestContent());
    RUVIA_CHECK(!stream.commitLocalEndStream());
}

RUVIA_TEST(http2_remote_receive_state_owns_head_content_connect_and_terminal_transitions) {
    std::pmr::monotonic_buffer_resource resource;

    Http2StreamState contentStream(1, &resource);
    const auto& content = contentStream.remoteReceive();
    RUVIA_CHECK(content.headPending() != nullptr);
    RUVIA_CHECK(contentStream.finalizeRemoteContentHead());
    RUVIA_CHECK(content.contentOpen() != nullptr);
    RUVIA_CHECK(contentStream.finishRemoteContent());
    RUVIA_CHECK(content.endStream() != nullptr);
    RUVIA_CHECK(!contentStream.finishRemoteContent());

    Http2StreamState headEnded(3, &resource);
    const auto& ended = headEnded.remoteReceive();
    RUVIA_CHECK(headEnded.recordRemoteHeadEndStream());
    RUVIA_CHECK(ended.headEndStreamPending() != nullptr);
    RUVIA_CHECK(headEnded.finalizeRemoteContentHead());
    RUVIA_CHECK(ended.endStream() != nullptr);

    Http2StreamState rejectedConnect(5, &resource);
    const auto& rejected = rejectedConnect.remoteReceive();
    RUVIA_CHECK(rejectedConnect.beginStandardConnect());
    RUVIA_CHECK(rejectedConnect.finalizeRemoteConnectHead());
    RUVIA_CHECK(rejected.connectPending() != nullptr);
    RUVIA_CHECK(rejectedConnect.rejectConnect());
    RUVIA_CHECK(rejected.connectRejectedAwaitingEndStream() != nullptr);
    RUVIA_CHECK(rejectedConnect.finishRemoteRejectedConnect());
    RUVIA_CHECK(rejected.endStream() != nullptr);

    Http2StreamState openTunnel(7, &resource);
    const auto& tunnel = openTunnel.remoteReceive();
    RUVIA_CHECK(openTunnel.beginExtendedConnect());
    RUVIA_CHECK(openTunnel.finalizeRemoteConnectHead());
    RUVIA_CHECK(openTunnel.acceptConnect());
    RUVIA_CHECK(tunnel.tunnelOpen() != nullptr);
    RUVIA_CHECK(openTunnel.finishRemoteTunnel());
    RUVIA_CHECK(tunnel.endStream() != nullptr);

    Http2StreamState halfClosedConnect(9, &resource);
    const auto& halfClosed = halfClosedConnect.remoteReceive();
    RUVIA_CHECK(halfClosedConnect.beginStandardConnect());
    RUVIA_CHECK(halfClosedConnect.recordRemoteHeadEndStream());
    RUVIA_CHECK(halfClosedConnect.finalizeRemoteConnectHead());
    RUVIA_CHECK(halfClosed.connectPendingEndStream() != nullptr);
    RUVIA_CHECK(halfClosedConnect.acceptConnect());
    RUVIA_CHECK(halfClosed.endStream() != nullptr);
}

RUVIA_TEST(stream_lifecycle_abort_sets_all_terminal_state) {
    std::pmr::monotonic_buffer_resource resource;
    Http2StreamState stream(1, &resource);
    RUVIA_CHECK(!stream.isAborted());
    RUVIA_CHECK(stream.remoteReceive().headPending() != nullptr);
    RUVIA_CHECK(stream.localSend().headPending() != nullptr);
    RUVIA_CHECK(!stream.abort(
        static_cast<Http2StreamCloseSource>(0xFF)));
    RUVIA_CHECK(stream.localSend().headPending() != nullptr);

    RUVIA_CHECK(stream.abort(Http2StreamCloseSource::kPeer));
    RUVIA_CHECK(stream.isAborted());
    RUVIA_CHECK(stream.remoteReceive().aborted() != nullptr);
    const auto* aborted = stream.localSend().aborted();
    RUVIA_CHECK(aborted != nullptr);
    RUVIA_CHECK(aborted != nullptr &&
        aborted->source() == Http2StreamCloseSource::kPeer);
    RUVIA_CHECK(!stream.abort(Http2StreamCloseSource::kPeerGoaway));
    RUVIA_CHECK(stream.localSend().aborted()->source() ==
        Http2StreamCloseSource::kPeer);
}

RUVIA_TEST(stream_lifecycle_abort_blocks_queue_and_dispatch) {
    std::pmr::monotonic_buffer_resource resource;
    Http2StreamState lifecycle(1, &resource);
    RUVIA_CHECK(lifecycle.tryMarkQueued());
    RUVIA_CHECK(lifecycle.queued());
    RUVIA_CHECK(!lifecycle.tryMarkQueued());
    lifecycle.clearQueued();
    RUVIA_CHECK(!lifecycle.queued());
    RUVIA_CHECK(lifecycle.tryMarkQueued());
    RUVIA_CHECK(lifecycle.tryStartDispatch());
    RUVIA_CHECK(!lifecycle.queued());
    RUVIA_CHECK(lifecycle.dispatchStarted());
    RUVIA_CHECK(!lifecycle.tryStartDispatch());

    Http2StreamState abortedFirst(3, &resource);
    RUVIA_CHECK(abortedFirst.abort(Http2StreamCloseSource::kLocal));
    RUVIA_CHECK(abortedFirst.remoteReceive().aborted() != nullptr);
    RUVIA_CHECK(!abortedFirst.tryMarkQueued());
    RUVIA_CHECK(!abortedFirst.tryStartDispatch());

    Http2StreamState queuedThenAborted(5, &resource);
    RUVIA_CHECK(queuedThenAborted.tryMarkQueued());
    RUVIA_CHECK(queuedThenAborted.abort(Http2StreamCloseSource::kPeer));
    RUVIA_CHECK(!queuedThenAborted.queued());
    RUVIA_CHECK(queuedThenAborted.remoteReceive().aborted() != nullptr);
    RUVIA_CHECK(!queuedThenAborted.tryStartDispatch());
}
