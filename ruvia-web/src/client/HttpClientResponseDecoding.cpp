#include "ruvia/web/detail/client/HttpClientResponseDecoding.h"

#include <algorithm>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/HttpContentCodec.h"
#include "ruvia/http/HttpContentCoding.h"
#include "ruvia/web/HttpClientTypes.h"
#include "ruvia/web/detail/client/HttpClientResponseState.h"

namespace ruvia::detail {

void configureHttpClientResponseDecoding(HttpClientResponseState& state) {
    state.bodyDecodeRequired = false;
    const auto* plan = state.responseBodyPlan ? &*state.responseBodyPlan : nullptr;
    if (plan == nullptr || plan->bodySuppressed() || !plan->statusAllowsBody() ||
        plan->contentSemantics() != HttpResponseContentSemantics::kWithContent) {
        return;
    }
    const auto parsedCoding = parseHttpContentCodingHeaders(state.headers, state.resource);
    const auto codings = parsedCoding.codings();
    const bool transformsBody =
        std::ranges::any_of(codings, [](HttpContentCoding coding) {
            return coding != HttpContentCoding::kIdentity;
        });
    state.bodyDecodeRequired = parsedCoding.invalid() != nullptr ||
                               parsedCoding.unsupported() != nullptr || transformsBody;
    if (state.bodyDecodeRequired) {
        state.collectAll = true;
    }
}

void decodeHttpClientResponseContentEncoding(HttpClientResponseState& state,
    bool contentSemanticsPresent, std::size_t maxDecodedBytes) {
    auto* const resource = state.resource;
    if (!contentSemanticsPresent || !state.bodyDecodeRequired) {
        return;
    }
    const auto* plan = state.responseBodyPlan ? &*state.responseBodyPlan : nullptr;
    if (plan == nullptr || plan->bodySuppressed() || !plan->statusAllowsBody() ||
        plan->contentSemantics() != HttpResponseContentSemantics::kWithContent) {
        return;
    }
    if (resource == nullptr || state.offset > state.buffered.size()) {
        throw std::invalid_argument("HTTP response decoder requires valid worker-owned storage");
    }

    try {
        const auto parsedCoding = parseHttpContentCodingHeaders(state.headers, resource);
        const auto codings = parsedCoding.codings();
        if (parsedCoding.invalid() != nullptr || parsedCoding.unsupported() != nullptr) {
            throw HttpClientError(HttpClientError::Code::kProtocolError,
                "unsupported HTTP response Content-Encoding");
        }
        const bool transformsBody =
            std::ranges::any_of(codings, [](HttpContentCoding coding) {
                return coding != HttpContentCoding::kIdentity;
            });
        if (!transformsBody) {
            state.bodyDecodeRequired = false;
            return;
        }

        const auto bufferedBytes = state.buffered.size() - state.offset;
        if (state.pending.size() > std::numeric_limits<std::size_t>::max() - bufferedBytes) {
            throw HttpClientError(HttpClientError::Code::kResponseTooLarge,
                "HTTP response exceeds configured byte limit");
        }
        std::pmr::string encoded(resource);
        encoded.reserve(bufferedBytes + state.pending.size());
        encoded.append(state.buffered.data() + state.offset, bufferedBytes);
        encoded.append(state.pending);

        const auto decodedLimit = std::min(maxDecodedBytes, state.bufferedLimit);
        auto decoded = decodeHttpContent(
            codings, encoded, {.maxDecodedBytes = decodedLimit, .resource = resource});
        if (auto* content = decoded.decoded()) {
            auto decodedBody = std::move(*content).takeBytes();
            if (!state.replaceProducerBodyBytes(decodedBody.size())) {
                throw HttpClientError(HttpClientError::Code::kResponseTooLarge,
                    "HTTP response exceeds configured byte limit");
            }
            std::pmr::string emptyBuffered(state.resource);
            std::pmr::string emptyPending(state.resource);
            state.buffered.swap(emptyBuffered);
            state.pending.swap(emptyPending);
            state.buffered.swap(decodedBody);
            state.offset = 0;
            state.bodyDecodeRequired = false;
            state.notifyProducerSpace();
            return;
        }
        const auto* failure = decoded.failure();
        if (failure != nullptr && failure->error() == HttpContentDecodeError::kDecodedSizeExceeded) {
            throw HttpClientError(HttpClientError::Code::kResponseTooLarge,
                "HTTP response exceeds configured byte limit");
        }
        if (failure != nullptr && failure->error() == HttpContentDecodeError::kDecoderFailure) {
            throw HttpClientError(HttpClientError::Code::kProtocolError,
                "HTTP response content-coding decoder failed");
        }
        throw HttpClientError(HttpClientError::Code::kProtocolError,
            "invalid HTTP response Content-Encoding");
    } catch (...) {
        // Encoded bytes were never exposed: the head selected collection before
        // waking any consumer. Release that retained input on every decode error.
        state.discardResponseBody();
        throw;
    }
}

}  // namespace ruvia::detail
