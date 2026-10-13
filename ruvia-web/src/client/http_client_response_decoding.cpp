#include "client/http_client_response_decoding.h"

#include <algorithm>
#include <limits>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/web/http_client_types.h"

#include "client/http_client_response_state.h"

namespace ruvia::detail {

void configure_http_client_response_decoding(http_client_response_state& state_value) {
    state_value.body_decode_required_ = false;
    state_value.content_coding_.reset();
    const auto* plan = state_value.response_body_plan_ ? &*state_value.response_body_plan_ : nullptr;
    // A 206 body (single range or multipart/byteranges) holds slices of the
    // encoded representation that cannot be decoded alone (RFC 9110 §14,
    // §15.3.7); it is exposed as received, with Content-Encoding kept.
    if (plan == nullptr || plan->body_suppressed() || !plan->status_allows_body() ||
        plan->content_semantics() != http_response_content_semantics::with_content ||
        state_value.status_ == http_status::partial_content) {
        return;
    }
    // Parsed once with the final head; decoding reuses this result at FIN.
    auto& parsed_coding = state_value.content_coding_.emplace(
        parse_http_content_coding_headers(state_value.headers_, state_value.resource_));
    const bool transforms_body = std::ranges::any_of(parsed_coding.codings(), [](http_content_coding coding) {
        return coding != http_content_coding::identity;
    });
    state_value.body_decode_required_ =
        parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr || transforms_body;
    if (state_value.body_decode_required_) {
        state_value.collect_all_ = true;
    } else {
        state_value.content_coding_.reset();
    }
}

void decode_http_client_response_body(http_client_response_state& state_value,
    bool content_semantics_present, std::size_t max_decoded_bytes) {
    if (!content_semantics_present || !state_value.body_decode_required_) {
        return;
    }
    auto* const resource = state_value.resource_;
    try {
        const auto& parsed_coding = *state_value.content_coding_;
        if (parsed_coding.invalid() != nullptr || parsed_coding.unsupported() != nullptr) {
            throw http_client_error(http_client_error::code_type::protocol_error,
                "unsupported HTTP response Content-Encoding");
        }
        // Encoded bytes are never exposed, so the unread part of buffered_ and
        // pending_ together are the whole body. Decode in place when one holds it.
        const auto buffered_bytes = state_value.buffered_.size() - state_value.offset_;
        std::pmr::string joined(resource);
        std::string_view encoded;
        if (state_value.pending_.empty()) {
            encoded = std::string_view(state_value.buffered_).substr(state_value.offset_);
        } else if (buffered_bytes == 0) {
            encoded = state_value.pending_;
        } else {
            if (state_value.pending_.size() > std::numeric_limits<std::size_t>::max() - buffered_bytes) {
                throw http_client_error(http_client_error::code_type::response_too_large,
                    "HTTP response exceeds configured byte limit");
            }
            joined.reserve(buffered_bytes + state_value.pending_.size());
            joined.append(state_value.buffered_.data() + state_value.offset_, buffered_bytes);
            joined.append(state_value.pending_);
            encoded = joined;
        }

        const auto decoded_limit = std::min(max_decoded_bytes, state_value.buffered_limit_);
        auto decoded = decode_http_content(
            parsed_coding.codings(), encoded, {.max_decoded_bytes_ = decoded_limit, .resource_ = resource});
        if (auto* content = decoded.decoded()) {
            auto decoded_body = std::move(*content).take_bytes();
            std::pmr::string empty_buffered(state_value.resource_);
            std::pmr::string empty_pending(state_value.resource_);
            state_value.buffered_.swap(empty_buffered);
            state_value.pending_.swap(empty_pending);
            state_value.buffered_.swap(decoded_body);
            state_value.offset_ = 0;
            state_value.body_decode_required_ = false;
            state_value.content_coding_.reset();
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
        state_value.content_coding_.reset();
        state_value.discard_response_body();
        throw;
    }
}

}  // namespace ruvia::detail
