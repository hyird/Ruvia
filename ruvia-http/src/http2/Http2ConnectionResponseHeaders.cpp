#include "ruvia/http/detail/http2/Http2Connection.h"

#include <charconv>
#include <cstdint>
#include <string_view>

#include "ruvia/http/HttpStatus.h"
#include "ruvia/http/detail/coding/HttpContentLength.h"
#include "ruvia/http/detail/field/HttpInterimResponseValidation.h"
#include "ruvia/http/detail/coding/HttpResponseContentSemantics.h"
#include "ruvia/http/detail/response/HttpResponseHeaderBits.h"
#include "ruvia/http/detail/response/HttpResponseKnownHeaders.h"
#include "ruvia/http/detail/http2/hpack/Http2HeaderBlock.h"
#include "ruvia/http/detail/http2/message/Http2HeaderRules.h"
#include "ruvia/http/detail/http2/message/Http2RemoteReceiveSemantics.h"
#include "ruvia/http/detail/http2/message/Http2RequestHeaders.h"
#include "ruvia/http/detail/http2/message/Http2ResponseHeaders.h"

// Decoding a response head as the client: ':status' first and once, the interim
// (1xx) budget, and which regular headers a decoded head may carry into the
// stream's table.

namespace ruvia::detail {

namespace {

// RFC 9113 §8.1: at most this many 1xx interim heads before the final response head
// (DoS bound; mirrors the retired client session's limit).
constexpr std::uint8_t kMaxHttp2InterimResponses = 8;

struct Http2ResponseDecodeContext final {
    explicit Http2ResponseDecodeContext(Http2StreamState& stream, Http2StreamHeaderDecodeTransaction* transaction) noexcept
        : base(stream, transaction),
          interimHeaders(HttpFieldListRole::kRecipient) {}

    Http2HeaderDecodeContext base;
    HttpInterimResponseHeaderValidator interimHeaders;
    std::optional<HttpStatusCode> status;
    bool sawRegular{false};
};

// Client-role response head decode: ':status' once and first, then validated regular
// headers into the stream's header table (1xx heads are validated but not stored).
bool http2OnDecodedResponseHeader(void* target, std::string_view name, std::string_view value) {
    auto* context = static_cast<Http2ResponseDecodeContext*>(target);
    if (!http2AccumulateHeaderListBytes(context->base, name, value)) {
        return false;
    }
    auto& stream = context->base.stream;
    if (name.empty()) {
        return false;
    }
    if (name.front() == ':') {
        if (name != ":status" || context->status || context->sawRegular) {
            return false;
        }
        int parsedStatus = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsedStatus);
        if (value.size() != 3 || ec != std::errc{} || ptr != value.data() + value.size()) {
            return false;
        }
        const auto status = HttpStatusCode::tryFromValue(static_cast<std::uint16_t>(parsedStatus));
        if (!status || *status == http_status::kSwitchingProtocols) {
            return false;
        }
        context->status = *status;
        return true;
    }
    if (!context->status || !http2IsValidDecodedResponseHeader(name, value)) {
        return false;
    }
    if (!context->base.acceptRegularField()) {
        return false;
    }
    context->sawRegular = true;
    if (context->status->isInformational()) {
        // Interim fields are validated but not stored. The shared incremental
        // validator keeps receive acceptance identical to both response writers.
        return context->interimHeaders.validate(name, value) == HttpInterimResponseHeaderValidationStatus::kOk;
    }
    const auto kind = classifyRequestHeader(name);
    const auto responseKnownBit = classifyResponseHeaderName(name);
    const auto responseContentSemantics = httpResponseContentSemantics(stream.requestKnownMethod(), *context->status);
    const bool successfulConnect = responseContentSemantics == HttpResponseContentSemantics::kConnectTunnel;
    if (responseKnownBit == kResponseHeaderContentLength && successfulConnect) {
        // RFC 9110 9.3.6: a client ignores Content-Length on a successful CONNECT
        // response. It describes neither HTTP content nor the following tunnel DATA.
        return true;
    }
    if (responseKnownBit == kResponseHeaderContentType) {
        if (!isValidHttpContentTypeFieldValue(value) || !stream.markSingletonResponseHeader(responseKnownBit)) {
            return false;
        }
    }
    if (responseKnownBit == kResponseHeaderContentEncoding && !isValidHttpContentEncodingFieldValue(value, HttpFieldListRole::kRecipient)) {
        return false;
    }
    if (name == "trailer" && !isValidHttpResponseTrailerFieldValue(value, HttpFieldListRole::kRecipient)) {
        return false;
    }
    if (responseKnownBit == kResponseHeaderContentLength) {
        HttpContentLengthState contentLength;
        if (contentLength.parseField(value) != HttpContentLengthParseStatus::kOk) {
            return false;
        }
        if (!stream.declareRemoteContentLength(*contentLength.value())) {
            return false;
        }
    }
    return stream.appendRequestHeader(name, value, kind);
}

}  // namespace

bool http2OnDecodedResponseTrailer(void* target, std::string_view name, std::string_view value) {
    auto& context = *static_cast<Http2HeaderDecodeContext*>(target);
    if (!http2AccumulateHeaderListBytes(context, name, value)) {
        return false;
    }

    // Response trailers have different field semantics from request trailers.
    // Reuse the same response-specific permission table that proves outbound
    // trailer sections, after applying HTTP/2's lowercase and connection-field
    // rules. In particular, Accept-Ranges and ETag are explicitly trailer-safe,
    // while response controls such as Date and Location are not.
    return context.acceptRegularField() && http2IsValidDecodedResponseHeader(name, value) &&
        !isForbiddenResponseTrailerName(name) && context.stream.appendRequestHeader(name, value, classifyRequestHeader(name));
}

HeaderDecodeStatus Http2Connection::decodeResponseHeaderBlock(Http2StreamState& stream, Http2StreamHeaderDecodeTransaction& streamTransaction, HpackDecoder::DecodeTransaction& hpackTransaction) {
    Http2ResponseDecodeContext context{stream, &streamTransaction};
    const auto result = decoder_.decode(stream.requestHeaderBlock(), &context, [](void* target, std::string_view name, std::string_view value) { return http2OnDecodedResponseHeader(target, name, value); }, hpackTransaction);
    if (const auto status = http2ClassifyHeaderDecodeResult(result); status != HeaderDecodeStatus::kOk) {
        return status;
    }
    if (!context.status) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (context.status->isInformational()) {
        // 1xx interim head cannot carry END_STREAM. Without it, the remote receive
        // state remains head-pending so the next HEADERS is decoded as another head.
        if (stream.remoteReceive().headEndStreamPending() != nullptr) {
            return HeaderDecodeStatus::kProtocolError;
        }
        stream.countInterimResponse();
        if (stream.interimResponseCount() > kMaxHttp2InterimResponses) {
            return HeaderDecodeStatus::kProtocolError;
        }
        return HeaderDecodeStatus::kOk;
    }
    if (!stream.setResponseStatus(*context.status)) {
        return HeaderDecodeStatus::kProtocolError;
    }
    const auto contentSemantics = httpResponseContentSemantics(stream.requestKnownMethod(), *context.status);
    // RFC 9110 section 15.3.6 gives 205 an ordinary, zero-length content
    // phase (unlike HEAD/204/304 representation metadata), but forbids a
    // server from generating any content. Bind that semantic limit into the
    // same byte-accounting state that validates DATA and Content-Length. A
    // successful CONNECT takes precedence because its following bytes are a
    // tunnel, not response content.
    if (*context.status == http_status::kResetContent && contentSemantics != HttpResponseContentSemantics::kConnectTunnel && !stream.declareRemoteContentLength(0)) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (contentSemantics == HttpResponseContentSemantics::kWithoutContent && !stream.selectRemoteContentMetadataOnly()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (stream.tunnel().pending() != nullptr) {
        if (contentSemantics == HttpResponseContentSemantics::kConnectTunnel) {
            if (!stream.acceptConnect()) {
                return HeaderDecodeStatus::kProtocolError;
            }
            stream.beginLocalContentUnbounded();
            (void)stream.openLocalConnectTunnel();
        } else {
            if (!stream.rejectConnect()) {
                return HeaderDecodeStatus::kProtocolError;
            }
            output_.appendFrame(Http2FrameType::kData, kHttp2FlagEndStream, stream.id(), {});
            (void)stream.rejectLocalConnect();
        }
    } else if (!stream.finalizeRemoteContentHead()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (http2RemotePeerHalfClosed(stream) && !stream.remoteContent().terminalLengthValid()) {
        return HeaderDecodeStatus::kProtocolError;
    }
    if (!stream.setResponseHeaderCount(stream.requestHeaderCount())) {
        return HeaderDecodeStatus::kProtocolError;
    }
    return HeaderDecodeStatus::kOk;
}

}  // namespace ruvia::detail
