#include "ruvia/web/detail/http3/Http3ResponseStreamSink.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "ruvia/http/Http3Frames.h"
#include "ruvia/http/HttpResponseStream.h"
#include "ruvia/web/detail/http3/Http3BufferedRequestDispatch.h"

namespace ruvia::detail {
Http3ResponseStreamSink::Http3ResponseStreamSink(Http3BufferedRequestDispatch& publisher,
    const WorkerHandle& worker, HttpKnownMethod method, http_response_stream_kind kind,
    std::pmr::memory_resource* resource, HttpResponseCodingSelection coding,
    HttpResponseCodingAvailability availability)
    : publisher_(publisher),
      worker_(worker),
      method_(method),
      kind_(kind),
      resource_(resource),
      compression_(resource, coding, availability) {}

bool Http3ResponseStreamSink::aborted() const noexcept {
    return state_.aborted() || publisher_.responseAborted();
}
void Http3ResponseStreamSink::requireActive() const {
    if (aborted()) {
        throw std::system_error(std::make_error_code(std::errc::operation_canceled));
    }
}
Task<TimerSleepResult> Http3ResponseStreamSink::sleep(std::chrono::milliseconds duration, const StopToken& stop) {
    requireActive();
    co_return co_await sleepFor(worker_, duration, stop);
}
Task<void> Http3ResponseStreamSink::commit(http_response_trailer_intent trailers) {
    requireActive();
    if (state_.committed()) {
        if (trailers == http_response_trailer_intent::present) {
            state_.ensureTrailersAllowed(http_response_stream_trailer_framing::http3_trailing_headers);
        }
        co_return;
    }
    auto response = co_await state_.streamingHead();
    requireActive();
    compression_.prepare(method_, response, kind_);
    auto prepared = publisher_.encodeStreamingResponseHead(std::move(response), method_, kind_, trailers);
    if (!prepared) {
        if (prepared.error().kind == Http3ResponseHeadError::peer_field_section_limit) {
            publisher_.reject_peer_field_section();
        }
        throw std::invalid_argument("invalid HTTP/3 streaming response head");
    }
    if (!publisher_.responseFieldSectionAllowed(prepared->head.field_section.decodedFieldSectionSize())) {
        publisher_.reject_peer_field_section();
    }
    data_.emplace(prepared->head.bodyPlan, prepared->head.declaredContentLength);
    compression_.activate(prepared->head.bodyPlan);
    // A handoff can accept a prefix before cancellation. Mark commitment before
    // entering that operation so recovery never emits a second response head.
    publisher_.commitFinalResponse();
    state_.markCommitted(prepared->commit_plan);
    co_await publisher_.publishResponseFrame(static_cast<std::uint64_t>(Http3FrameType::kHeaders), prepared->head.field_section.fieldSection);
    if (prepared->commit_plan.head_disposition() == http_response_stream_head_disposition::message_ended) {
        co_await publisher_.finishResponse();
    }
}
Task<void> Http3ResponseStreamSink::writeEncoded(std::string_view bytes) {
    constexpr std::size_t blockBytes = 16 * 1024;
    for (std::size_t offset = 0; offset < bytes.size();) {
        requireActive();
        const auto count = std::min(blockBytes, bytes.size() - offset);
        const auto planned = data_->planChunk(std::span<const char>(bytes.data() + offset, count), false);
        if (!planned) {
            throw std::length_error("HTTP/3 response content length exceeded");
        }
        co_await publisher_.publishResponseFrame(static_cast<std::uint64_t>(Http3FrameType::kData), planned->payload);
        if (!data_->commitPayload(count, false)) {
            std::terminate();
        }
        offset += count;
    }
}
Task<void> Http3ResponseStreamSink::write(std::string_view bytes) {
    requireActive();
    if (bytes.empty()) {
        co_return;
    }
    co_await commit(http_response_trailer_intent::none);
    if (state_.bodySuppressedComplete()) {
        co_await sleepFor(worker_, std::chrono::steady_clock::duration(1));
    }
    state_.ensureBodyAllowed();
    if (!compression_.active()) {
        co_await writeEncoded(bytes);
        co_return;
    }
    constexpr std::size_t blockBytes = 16 * 1024;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto count = std::min(blockBytes, bytes.size() - offset);
        try {
            compression_.write(bytes.substr(offset, count));
        } catch (...) {
            state_.markAborted();
            throw;
        }
        co_await writeEncoded(compression_.output());
        offset += count;
    }
}
Task<void> Http3ResponseStreamSink::end(std::span<const HttpHeaderView> trailers) {
    if (state_.ended()) {
        if (!trailers.empty()) {
            throw std::logic_error("HTTP/3 response stream already ended");
        }
        co_return;
    }
    requireActive();
    const auto section = validateHttpResponseTrailers(trailers);
    co_await commit(response_trailer_intent(section));
    if (state_.ended()) {
        co_return;
    }
    if (compression_.active()) {
        try {
            compression_.finish();
        } catch (...) {
            state_.markAborted();
            throw;
        }
        co_await writeEncoded(compression_.output());
    }
    const auto finishing = data_->planChunk({}, true);
    if (!finishing) {
        throw std::length_error("HTTP/3 response content length incomplete");
    }
    if (!trailers.empty()) {
        state_.ensureTrailersAllowed(http_response_stream_trailer_framing::http3_trailing_headers);
        std::pmr::vector<Http3FieldSectionFieldView> fields(resource_);
        for (const auto& field : trailers) {
            fields.push_back({field.name(), field.value(), false});
        }
        auto encoded = publisher_.encodeResponseTrailers(fields);
        if (!encoded) {
            if (encoded.error().kind == Http3ResponseHeadError::peer_field_section_limit) {
                publisher_.reject_peer_field_section();
            }
            throw std::invalid_argument("invalid HTTP/3 response trailers");
        }
        if (!publisher_.responseFieldSectionAllowed(encoded->decodedFieldSectionSize())) {
            publisher_.reject_peer_field_section();
        }
        co_await publisher_.publishResponseFrame(static_cast<std::uint64_t>(Http3FrameType::kHeaders), encoded->fieldSection);
    }
    co_await publisher_.finishResponse();
    if (!data_->commitPayload(0, true)) {
        std::terminate();
    }
    state_.markEnded();
}
}  // namespace ruvia::detail
