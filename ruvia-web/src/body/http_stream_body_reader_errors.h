#pragma once

#include <stdexcept>

#include "ruvia/http/http_request_body_failure.h"
#include "ruvia/http/http_transfer_coding_decoder.h"
namespace ruvia::detail {

[[noreturn]] inline void throw_request_body_too_large() {
    throw http_request_body_failure::too_large().protocol_error();
}

[[noreturn]] inline void throw_incomplete_request_body() {
    throw http_request_body_failure::incomplete().protocol_error();
}

[[noreturn]] inline void throw_transfer_coding_protocol_failure(
    const http_transfer_coding_decode_failure& failure) {
    throw http_request_transfer_coding_error(failure.error());
}

[[noreturn]] inline void throw_http_transfer_coding_decoder_failure() {
    throw std::runtime_error("transfer-coding decoder failure");
}

inline void require_complete_transfer_coding(http_transfer_coding_stack_decoder& decoder) {
    const auto finish_result = decoder.finish_input();
    if (finish_result.complete() != nullptr) {
        return;
    }
    if (const auto* failure = finish_result.failure()) {
        throw_transfer_coding_protocol_failure(*failure);
    }
    if (finish_result.decoder_failure() != nullptr) {
        throw_http_transfer_coding_decoder_failure();
    }
    throw std::logic_error("unexpected transfer-coding finish result");
}

}  // namespace ruvia::detail
