#pragma once

#include <stdexcept>

#include "ruvia/http/detail/coding/HttpTransferCodingDecoder.h"
#include "ruvia/http/detail/request/HttpRequestBodyFailure.h"

namespace ruvia::detail {

[[noreturn]] inline void throwRequestBodyTooLarge() {
    throw HttpRequestBodyFailure::tooLarge().protocolError();
}

[[noreturn]] inline void throwIncompleteRequestBody() {
    throw HttpRequestBodyFailure::incomplete().protocolError();
}

[[noreturn]] inline void throwTransferCodingProtocolFailure(
    const TransferCodingDecodeProtocolFailure& failure) {
    throw failure.protocolError();
}

[[noreturn]] inline void throwTransferCodingDecoderFailure() {
    throw std::runtime_error("transfer-coding decoder failure");
}

inline void requireCompleteTransferCoding(TransferCodingDecoder& decoder) {
    const auto finishResult = decoder.finishInput();
    if (finishResult.complete() != nullptr) {
        return;
    }
    if (const auto* failure = finishResult.protocolFailure()) {
        throwTransferCodingProtocolFailure(*failure);
    }
    if (finishResult.decoderFailure() != nullptr) {
        throwTransferCodingDecoderFailure();
    }
    throw std::logic_error("unexpected transfer-coding finish result");
}

}  // namespace ruvia::detail
