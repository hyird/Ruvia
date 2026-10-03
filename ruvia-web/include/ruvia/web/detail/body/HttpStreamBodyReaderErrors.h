#pragma once

#include <stdexcept>

#include "ruvia/http/HttpRequestBodyFailure.h"
#include "ruvia/http/HttpTransferCodingDecoder.h"
namespace ruvia::detail {

[[noreturn]] inline void throwRequestBodyTooLarge() {
    throw HttpRequestBodyFailure::tooLarge().protocolError();
}

[[noreturn]] inline void throwIncompleteRequestBody() {
    throw HttpRequestBodyFailure::incomplete().protocolError();
}

[[noreturn]] inline void throwTransferCodingProtocolFailure(
    const HttpTransferCodingDecodeFailure& failure) {
    throw httpRequestTransferCodingError(failure.error());
}

[[noreturn]] inline void throwHttpTransferCodingDecoderFailure() {
    throw std::runtime_error("transfer-coding decoder failure");
}

inline void require_complete_transfer_coding(http_transfer_coding_stack_decoder& decoder) {
    const auto finish_result = decoder.finish_input();
    if (finish_result.complete() != nullptr) {
        return;
    }
    if (const auto* failure = finish_result.failure()) {
        throwTransferCodingProtocolFailure(*failure);
    }
    if (finish_result.decoderFailure() != nullptr) {
        throwHttpTransferCodingDecoderFailure();
    }
    throw std::logic_error("unexpected transfer-coding finish result");
}

}  // namespace ruvia::detail
