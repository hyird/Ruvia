#include "HttpHeaderAccess.h"
#include "http2/Http2Connection.h"
#include "http2/Http2FramePayload.h"
#include "http2/Http2HeaderBlock.h"
#include "http2/Http2RemoteReceiveSemantics.h"

namespace ruvia::detail {

bool Http2Connection::processPushPromise(const Http2FrameHeader& header, std::string_view payload) {
    if (role_ != Http2Role::kClient || !enablePush_ || header.streamId == 0 || (header.streamId & 1U) == 0) {
        appendGoaway(Http2ErrorCode::kProtocolError, "PUSH_PROMISE not permitted");
        return false;
    }
    const auto* parent = findStream(header.streamId);
    const bool canceledParent = parent ? parent->isAborted() &&
                                             parent->localSend().aborted()->source() == Http2StreamCloseSource::kLocal
                                       : closedStreams_.source(header.streamId) == Http2StreamCloseSource::kLocal;
    if (!canceledParent && (!parent || http2RemotePeerHalfClosed(*parent))) {
        appendGoaway(Http2ErrorCode::kProtocolError, "PUSH_PROMISE on closed or idle stream");
        return false;
    }
    std::string_view fragment;
    if (http2StripPadAndPriority(header, payload, false, fragment) != Http2FramePayloadStatus::kDecoded) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid PUSH_PROMISE padding");
        return false;
    }
    if (fragment.size() < 4) {
        appendGoaway(Http2ErrorCode::kFrameSizeError, "missing promised stream id");
        return false;
    }
    const auto promised = http2Read31(reinterpret_cast<const unsigned char*>(fragment.data()));
    if (promised == 0 || (promised & 1U) != 0 || promised <= lastPeerPushStreamId_) {
        appendGoaway(Http2ErrorCode::kProtocolError, "invalid promised stream id");
        return false;
    }
    try {
        pushHeaderStream_.emplace(promised, resource_);
        pushAssociatedStreamId_ = header.streamId;
        if (!http2StartHeaderBlock(*pushHeaderStream_, fragment.substr(4))) {
            pushHeaderStream_.reset();
            appendGoaway(Http2ErrorCode::kCompressionError, "push field block exceeds limit");
            return false;
        }
        if ((header.flags & kHttp2FlagEndHeaders) != 0) {
            const auto result = finishPushPromise();
            pushHeaderStream_.reset();
            return result;
        }
        headerContinuation_.start(header.streamId, Http2HeaderBlockKind::kPushPromise);
        return true;
    } catch (...) {
        pushHeaderStream_.reset();
        throw;
    }
}

bool Http2Connection::processPushContinuation(const Http2FrameHeader& header, std::string_view payload) {
    if (!pushHeaderStream_) {
        appendGoaway(Http2ErrorCode::kProtocolError, "missing push continuation state");
        return false;
    }
    const auto checkpoint = headerContinuation_.checkpoint();
    const auto bytes = pushHeaderStream_->remoteHeaderBlock().size();
    try {
        if (!http2AppendHeaderBlock(*pushHeaderStream_, payload)) {
            appendGoaway(Http2ErrorCode::kCompressionError, "push field block exceeds limit");
            return false;
        }
        (void)headerContinuation_.recordContinuationFrame();
        if ((header.flags & kHttp2FlagEndHeaders) != 0) {
            const auto result = finishPushPromise();
            pushHeaderStream_.reset();
            headerContinuation_.reset();
            return result;
        }
        return true;
    } catch (...) {
        if (pushHeaderStream_) {
            pushHeaderStream_->remoteHeaderBlock().resize(bytes);
        }
        headerContinuation_.restore(checkpoint);
        throw;
    }
}

bool Http2Connection::finishPushPromise() {
    auto& scratch = *pushHeaderStream_;
    // PUSH_PROMISE represents a complete request without a request body.
    (void)scratch.recordRemoteHeadEndStream();
    Http2StreamHeaderDecodeTransaction streamTransaction{scratch, true};
    auto hpackTransaction = decoder_.beginTransaction();
    const auto status = decodeHeaderBlock(scratch, streamTransaction, hpackTransaction);
    if (status == HeaderDecodeStatus::kCompressionError) {
        appendGoaway(Http2ErrorCode::kCompressionError, "push field block decompression failed");
        return false;
    }
    if (status != HeaderDecodeStatus::kOk ||
        (scratch.requestMethod() != "GET" && scratch.requestMethod() != "HEAD")) {
        output_.appendRstStream(scratch.id(), Http2ErrorCode::kProtocolError);
        hpackTransaction.commit();
        streamTransaction.commit();
        lastPeerPushStreamId_ = scratch.id();
        lastStreamId_ = scratch.id();
        return true;
    }
    HttpPushRequest request(resource_);
    request.method = scratch.requestMethod();
    request.scheme = scratch.requestScheme();
    request.authority = scratch.requestAuthority();
    request.path = scratch.requestPath();
    request.headers.reserve(scratch.remoteHeaderCount() + static_cast<std::size_t>(scratch.hasCookie()));
    for (std::size_t i = 0; i < scratch.remoteHeaderCount(); ++i) {
        const auto field = scratch.remoteHeaderAt(i);
        request.headers.push_back(HttpHeaderAccess::make(field.name, field.value, resource_));
    }
    if (scratch.hasCookie()) {
        request.headers.push_back(HttpHeaderAccess::make("cookie", scratch.requestCookie(), resource_));
    }
    reserveEventSlots(1);
    const auto id = scratch.id();
    auto* stream = createStream(id);
    if (!stream) {
        output_.appendRstStream(id, Http2ErrorCode::kEnhanceYourCalm);
        hpackTransaction.commit();
        streamTransaction.commit();
        lastPeerPushStreamId_ = id;
        lastStreamId_ = id;
        return true;
    }
    try {
        stream->assignRequestMethod(request.method);
        stream->assignRequestScheme(request.scheme);
        stream->assignRequestAuthority(request.authority);
        stream->assignRequestPath(request.path);
        stream->beginLocalContentForbidden();
        (void)stream->commitLocalHeadEndStream();
        stream->reservePush(Http2PushReservation::kRemote);
        events_.push_back(Http2Event::pushPromise({pushAssociatedStreamId_, id, std::move(request)}));
    } catch (...) {
        streams_.remove(id);
        throw;
    }
    hpackTransaction.commit();
    streamTransaction.commit();
    lastPeerPushStreamId_ = id;
    lastStreamId_ = id;
    // Transactions borrow scratch until their destructors run.
    return true;
}

}  // namespace ruvia::detail
