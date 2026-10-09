#include "client/http_client_response_decoding.h"

#include <algorithm>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_response_state.h"

namespace ruvia::detail {

void configure_http_client_response_decoding(http_client_response_state& state_value) {
    state_value.body_decode_required_ = false;
    const auto* plan = state_value.response_body_plan_ ? &*state_value.response_body_plan_ : nullptr;
    if (plan == nullptr || plan->body_suppressed() || !plan->status_allows_body() ||
        plan->content_semantics() != http_response_content_semantics_type::with_content) {
        return;
    }
    const auto parsed_coding = parse_http_content_coding_headers(state_value.headers_, state_value.resource_);
    const auto codings = parsed_coding.codings();
    const bool transforms_body =
        std::ranges::any_of(codings, [](http_content_coding coding) {
            return coding != http_content_coding::identity;
        });
    state_value.body_decode_required_ = parsed_coding.invalid() != nullptr ||
                                        parsed_coding.unsupported() != nullptr || transforms_body;
    if (state_value.body_decode_required_) {
        state_value.collect_all_ = true;
    }
}

void decode_http_client_response_content_encoding(http_client_response_state& state_value,
    bool content_semantics_present, std::size_t max_decoded_bytes) {
    auto* const resource = state_value.resource_;
    if (!content_semantics_present || !state_value.body_decode_required_) {
        return;
    }
    const auto* plan = state_value.response_body_plan_ ? &*state_value.response_body_plan_ : nullptr;
    if (plan == nullptr || plan->body_suppressed() || !plan->status_allows_body() ||
        plan->content_semantics() != http_response_content_semantics_type::with_content) {
        return;
    }
    if (resource == nullptr || state_value.offset_ > state_value.buffered_.size()) {
        throw std::invalid_argument("HTTP response decoder requires valid worker-owned storage");
    }

    try {
        const auto parsed_coding = parse_http_content_coding_headers(state_value.headers_, resource);
        const auto codings = parsed_coding.codings();
        if (parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                "unsupported HTTP response Content-Encoding");
        }
        const bool transforms_body =
            std::ranges::any_of(codings, [](http_content_coding coding) {
                return coding != http_content_coding::identity;
            });
        if (!transforms_body) {
            state_value.body_decode_required_ = false;
            return;
        }

        const auto buffered_bytes = state_value.buffered_.size() - state_value.offset_;
        if (state_value.pending_.size() > std::numeric_limits<std::size_t>::max() - buffered_bytes) {
            throw http_client_error(http_client_error::code_type::response_too_large,
                "HTTP response exceeds configured byte limit");
        }
        std::pmr::string encoded(resource);
        encoded.reserve(buffered_bytes + state_value.pending_.size());
        encoded.append(state_value.buffered_.data() + state_value.offset_, buffered_bytes);
        encoded.append(state_value.pending_);

        const auto decoded_limit = std::min(max_decoded_bytes, state_value.buffered_limit_);
        auto decoded = decode_http_content(
            codings, encoded, {.max_decoded_bytes_ = decoded_limit, .resource_ = resource});
        if (auto* content = decoded.decoded()) {
            auto decoded_body = std::move(*content).take_bytes();
            if (!state_value.replace_producer_body_bytes(decoded_body.size())) {
                throw http_client_error(http_client_error::code_type::response_too_large,
                    "HTTP response exceeds configured byte limit");
            }
            std::pmr::string empty_buffered(state_value.resource_);
            std::pmr::string empty_pending(state_value.resource_);
            state_value.buffered_.swap(empty_buffered);
            state_value.pending_.swap(empty_pending);
            state_value.buffered_.swap(decoded_body);
            state_value.offset_ = 0;
            state_value.body_decode_required_ = false;
            state_value.notify_producer_space();
            return;
        }
        const auto* failure = decoded.failure();
        if (failure != nullptr && failure->error() == http_content_decode_error::decoded_size_exceeded) {
            throw http_client_error(http_client_error::code_type::response_too_large,
                "HTTP response exceeds configured byte limit");
        }
        if (failure != nullptr && failure->error() == http_content_decode_error::decoder_failure) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                "HTTP response content-coding decoder failed");
        }
        throw http_client_error(http_client_error::code_type::protocol_error,
            "invalid HTTP response Content-Encoding");
    } catch (...) {
        // Encoded bytes were never exposed: the head selected collection before
        // waking any consumer. Release that retained input on every decode error.
        state_value.discard_response_body();
        throw;
    }
}

}  // namespace ruvia::detail
