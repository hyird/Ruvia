#include "ruvia/http/detail/http2/Http2Connection.h"

#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/HttpRequestContentSemantics.h"
#include "ruvia/http/detail/http2/Http2HeaderBlock.h"
#include "ruvia/http/detail/http2/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/Http2RequestHeaders.h"
#include "ruvia/http/detail/http2/Http2ResponseHeaders.h"

namespace ruvia::detail {

HeaderDecodeStatus Http2Connection::decodeHeaderBlock(Http2StreamState& stream) {
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(), &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedInitialHeader(
                *static_cast<Http2HeaderDecodeContext*>(target), name, value);
        });
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!stream.hasMethod()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.hasProtocol()) {
        if (prefacePhase_ != PrefacePhase::kReady ||
            stream.requestKnownMethod() != HttpKnownMethod::kConnect ||
            !stream.hasScheme() ||
            !stream.hasPath() ||
            !stream.hasAuthority() ||
            !http2IsValidRequestAuthority(
                stream.requestScheme(), stream.requestAuthority()) ||
            !http2IsValidExtendedConnectPath(
                stream.requestScheme(), stream.requestPath()) ||
            stream.remoteContent().allowedKnownLength() != nullptr) {
            return HeaderDecodeStatus::kProtocolError;
        }
        // RFC 8441 defines WebSocket extended CONNECT only for HTTP(S) URI
        // schemes. Other extended protocols retain the full RFC 3986 space.
        if (stream.protocolIsWebSocket() &&
            !httpAsciiEqualsIgnoreCase(stream.requestScheme(), "http") &&
            !httpAsciiEqualsIgnoreCase(stream.requestScheme(), "https")) {
            return HeaderDecodeStatus::kProtocolError;
        }
        if (!stream.beginExtendedConnect()) {
            return HeaderDecodeStatus::kProtocolError;
        }
    } else if (stream.requestKnownMethod() == HttpKnownMethod::kConnect) {
        RequestTargetView connectTarget;
        if (!stream.hasAuthority() || stream.hasScheme() || stream.hasPath() ||
            stream.remoteContent().allowedKnownLength() != nullptr ||
            !parseRequestTarget(
                HttpKnownMethod::kConnect,
                stream.requestAuthority(),
                connectTarget)) {
            return HeaderDecodeStatus::kProtocolError;
        }
        if (!stream.beginStandardConnect()) {
            return HeaderDecodeStatus::kProtocolError;
        }
    } else if (!stream.hasScheme() || !stream.hasPath() ||
        (stream.hasAuthority() && !http2IsValidRequestAuthority(
            stream.requestScheme(), stream.requestAuthority())) ||
        !http2IsValidRegularRequestPath(
            stream.requestKnownMethod(),
            stream.requestScheme(),
            stream.requestPath()) ||
        (stream.requestPath() == "*" && stream.hasAuthority()) ||
        (!stream.hasAuthority() && http2RegularRequestRequiresAuthority(
            stream.requestScheme(), stream.requestPath()))) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (role_ == Http2Role::kServer &&
        stream.requestKnownMethod() != HttpKnownMethod::kConnect) {
        const auto contentSemantics =
            httpRequestContentSemantics(stream.requestMethod());
        if (contentSemantics ==
            HttpRequestContentSemantics::kForbidden) {
            // A declared length is an explicit content signal, including zero.
            // Without a length, retain the open remote half for a legal empty
            // DATA(END_STREAM), but make non-empty DATA unrepresentable as content.
            if (stream.remoteContent().allowedKnownLength() != nullptr ||
                !stream.selectRemoteContentMetadataOnly()) {
                return HeaderDecodeStatus::kProtocolError;
            }
        } else if (contentSemantics ==
            HttpRequestContentSemantics::kContentTypeRequired) {
            // A declared length (including zero) or an open peer send half is
            // explicit OPTIONS content in the same cases modeled by the HTTP/2
            // request writer. RFC 9110 section 9.3.7 requires a valid media type.
            const bool explicitContent =
                stream.remoteContent().allowedKnownLength() != nullptr ||
                stream.remoteReceive().headPending() != nullptr;
            if (explicitContent &&
                !stream.hasSingletonRequestHeader(
                    singletonRequestHeaderBit(
                        RequestHeaderKind::kContentType))) {
                return HeaderDecodeStatus::kProtocolError;
            }
        }
    }
    const bool remoteHeadFinalized = stream.tunnel().pending() != nullptr
        ? stream.finalizeRemoteConnectHead()
        : stream.finalizeRemoteContentHead();
    if (!remoteHeadFinalized) {
        return HeaderDecodeStatus::kProtocolError;
    }
    // NOTE (sans-I/O): resolveStreamRoute is deliberately NOT called here -- route
    // resolution and body-mode selection are application policy the owner applies
    // after pulling the kMessageHead event.
    return HeaderDecodeStatus::kOk;
}

HeaderDecodeStatus Http2Connection::decodeInitialHeaderBlock(Http2StreamState& stream) {
    return role_ == Http2Role::kClient ? decodeResponseHeaderBlock(stream) : decodeHeaderBlock(stream);
}

HeaderDecodeStatus Http2Connection::decodeRefusedHeaderBlock(Http2StreamState& stream) {
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(), &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2OnDecodedInitialHeader(
                *static_cast<Http2HeaderDecodeContext*>(target), name, value);
        });
    http2ResetHeaderBlock(stream);
    return http2ClassifyHeaderDecodeResult(result);
}

HeaderDecodeStatus Http2Connection::decodeDiscardedHeaderBlock(Http2StreamState& stream) {
    // Even a block whose HTTP semantics are no longer observable must be decoded in
    // full because HPACK's dynamic table is connection-scoped. Keep the decompressed
    // field-list budget, but deliberately avoid mutating request/response state.
    Http2HeaderDecodeContext context{stream};
    const auto result = decoder_.decode(
        stream.requestHeaderBlock(), &context,
        [](void* target, std::string_view name, std::string_view value) {
            return http2AccumulateHeaderListBytes(
                *static_cast<Http2HeaderDecodeContext*>(target), name, value);
        });
    http2ResetHeaderBlock(stream);
    return http2ClassifyHeaderDecodeResult(result);
}

bool Http2Connection::startDiscardedHeaderBlock(
    const Http2FrameHeader& header,
    std::string_view fragment,
    DiscardedHeaderAction action) {
    if (discardedHeaderStream_) {
        appendGoaway(Http2ErrorCode::kProtocolError, "overlapping discarded HEADERS block");
        return false;
    }
    discardedHeaderStream_.emplace(header.streamId, resource_);
    discardedHeaderAction_ = action;
    if (!http2StartHeaderBlock(*discardedHeaderStream_, fragment)) {
        discardedHeaderStream_.reset();
        discardedHeaderAction_ = DiscardedHeaderAction::kIgnore;
        appendGoaway(Http2ErrorCode::kCompressionError, "field block not decompressed");
        return false;
    }
    if ((header.flags & kHttp2FlagEndHeaders) != 0) {
        return finishDiscardedHeaderBlock();
    }
    headerContinuation_.start(header.streamId, Http2HeaderBlockKind::kDiscarded);
    return true;
}

bool Http2Connection::finishDiscardedHeaderBlock() {
    if (!discardedHeaderStream_) {
        appendGoaway(Http2ErrorCode::kProtocolError, "missing discarded HEADERS state");
        return false;
    }
    const auto streamId = discardedHeaderStream_->id();
    const auto action = discardedHeaderAction_;
    const auto status = action == DiscardedHeaderAction::kRefuseStream
        ? decodeRefusedHeaderBlock(*discardedHeaderStream_)
        : decodeDiscardedHeaderBlock(*discardedHeaderStream_);
    discardedHeaderStream_.reset();
    discardedHeaderAction_ = DiscardedHeaderAction::kIgnore;

    if (status == HeaderDecodeStatus::kCompressionError) {
        appendGoaway(Http2ErrorCode::kCompressionError, "invalid HPACK block");
        return false;
    }
    if (action == DiscardedHeaderAction::kIgnore) {
        return true;
    }

    auto error = Http2ErrorCode::kProtocolError;
    if (action == DiscardedHeaderAction::kResetStreamClosed) {
        error = Http2ErrorCode::kStreamClosed;
    } else if (action == DiscardedHeaderAction::kRefuseStream &&
               status == HeaderDecodeStatus::kOk) {
        error = Http2ErrorCode::kRefusedStream;
    }
    output_.appendRstStream(streamId, error);
    if (findStream(streamId) != nullptr) {
        closeStream(streamId, Http2StreamCloseSource::kLocal, error);
    } else {
        closedStreams_.remember(streamId, Http2StreamCloseSource::kLocal);
    }
    return true;
}

HeaderDecodeStatus Http2Connection::finishTrailerBlock(Http2StreamState& stream) {
    Http2HeaderDecodeContext context{stream};
    const auto result = role_ == Http2Role::kServer
        ? decoder_.decode(
              stream.requestHeaderBlock(),
              &context,
              [](void* target, std::string_view name, std::string_view value) {
                  return http2OnDecodedRequestTrailer(
                      *static_cast<Http2HeaderDecodeContext*>(target),
                      name,
                      value);
              })
        : decoder_.decode(
              stream.requestHeaderBlock(),
              &context,
              http2OnDecodedResponseTrailer);
    http2ResetHeaderBlock(stream);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!stream.remoteContent().terminalLengthValid()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (!stream.finishRemoteContent()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    events_.push_back(Http2Event::messageEnd(stream.id()));
    releaseLocalRequestStreamIfClosed(stream);
    return HeaderDecodeStatus::kOk;
}

bool Http2Connection::handleHeaderDecodeFailure(Http2StreamState& stream, HeaderDecodeStatus status) {
    if (status == HeaderDecodeStatus::kCompressionError) {
        appendGoaway(Http2ErrorCode::kCompressionError, "invalid HPACK block");
        return false;
    }
    output_.appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
    if (findStream(stream.id()) == &stream) {
        closeStream(
            stream.id(),
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kProtocolError);  // emits a typed stream-closed event
    } else {
        (void)stream.abort(Http2StreamCloseSource::kLocal);
        // Refused-stream scratch is not in the table, but still models the same
        // whole-stream terminal transition as a live stream.
    }
    return true;
}

void Http2Connection::emitRequestHeaders(Http2StreamState& stream) {
    // RFC 9113 §8.1.1: a declared content-length must equal the summed DATA payload.
    // A body-less HEADERS (END_STREAM set) with a nonzero content-length can never be
    // satisfied -- reject it here so both END_STREAM routes stay consistent. Client
    // role: HEAD responses and 204/304 are exempt (their content-length describes the
    // body a GET would have had, RFC 9110 §8.6).
    if (http2RemotePeerHalfClosed(stream) &&
        !stream.remoteContent().terminalLengthValid()) {
        output_.appendRstStream(stream.id(), Http2ErrorCode::kProtocolError);
        closeStream(
            stream.id(),
            Http2StreamCloseSource::kLocal,
            Http2ErrorCode::kProtocolError);  // remove, don't leak the slot
        return;
    }
    events_.push_back(Http2Event::messageHead(stream.id()));
    if (role_ == Http2Role::kServer &&
        stream.tunnel().pending() != nullptr) {
        // CONNECT has no request content, but that fact alone says nothing about the
        // peer send half: it can remain open for a tunnel or already carry END_STREAM.
        // Route/accept decisions start from kMessageHead; the typed remote state owns
        // any later tunnel-end signal, so a generic kMessageEnd would be misleading.
        return;
    }
    if (role_ == Http2Role::kClient &&
        stream.tunnel().open() != nullptr) {
        if (http2RemotePeerHalfClosed(stream)) {
            events_.push_back(Http2Event::tunnelEnd(stream.id()));
        }
        releaseLocalRequestStreamIfClosed(stream);
        return;
    }
    if (http2RemotePeerHalfClosed(stream)) {
        events_.push_back(Http2Event::messageEnd(stream.id()));
    }
    releaseLocalRequestStreamIfClosed(stream);
}

}  // namespace ruvia::detail
