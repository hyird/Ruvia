#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/detail/coding/HttpAcceptEncoding.h"
#include "ruvia/http/detail/coding/HttpContentEncoder.h"
#include "ruvia/http/detail/server/HttpResponseStreamHead.h"
#include "ruvia/web/Error.h"
#include "ruvia/web/detail/server/response/HttpResponseCompression.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/core/memory/PmrResource.h"

namespace ruvia::detail {

// Owns the response-coding lifecycle shared by HTTP/1 and HTTP/2 stream sinks.
// The transport only submits output(); it never reconstructs whether a coding
// was selected, whether the body is suppressed, or whether an encoder exists.
class HttpStreamingResponseCompression final {
public:
    HttpStreamingResponseCompression(std::pmr::memory_resource* resource,
        HttpResponseCodingSelection selection, HttpResponseCodingAvailability availability) noexcept
        : selection_(selection),
          availability_(availability),
          encodedChunk_(pmrResourceOrDefault(resource)) {}

    HttpStreamingResponseCompression(const HttpStreamingResponseCompression&) = delete;
    HttpStreamingResponseCompression& operator=(const HttpStreamingResponseCompression&) = delete;
    HttpStreamingResponseCompression(HttpStreamingResponseCompression&&) = delete;
    HttpStreamingResponseCompression& operator=(HttpStreamingResponseCompression&&) = delete;

    // Applies the representation headers before the protocol-owned stream head
    // is committed. A forbidden identity fallback is rejected here, while the
    // encoder itself is delayed until activate() has a final body plan.
    void prepare(HttpKnownMethod requestMethod, HttpResponse& response, ResponseStreamKind kind) {
        if (!std::holds_alternative<Unprepared>(state_)) {
            throw std::logic_error("streaming response compression is already prepared");
        }
        if (selection_.coding() == HttpContentCoding::kIdentity) {
            // Identity is the negotiated choice, but an application-provided
            // Content-Encoding is still a separate representation claim. Do
            // not let this early identity path bypass its acceptability check.
            if (httpResponseCodingFallbackForbidden(selection_, requestMethod, response)) {
                throw HttpError({.status = ruvia::http_status::kNotAcceptable,
                    .code = "not_acceptable",
                    .message = "no acceptable response content coding"});
            }
            state_.emplace<Identity>();
            return;
        }

        // Negotiation is intentionally complete before the representation
        // source is known: a static sidecar may already provide the selected
        // coding even when this process cannot create a new encoder. Only the
        // stream representation policy may turn that capability limitation
        // into an identity fallback or 406.
        if (availability_ == HttpResponseCodingAvailability::kIdentityOnly) {
            if (httpResponseCodingFallbackForbidden(selection_, requestMethod, response)) {
                throw HttpError({.status = ruvia::http_status::kNotAcceptable,
                    .code = "not_acceptable",
                    .message = "no acceptable response content coding"});
            }
            state_.emplace<Identity>();
            return;
        }

        if (!prepareStreamingResponseCompression(selection_, requestMethod, response, kind)) {
            if (httpResponseCodingFallbackForbidden(selection_, requestMethod, response)) {
                throw HttpError({.status = ruvia::http_status::kNotAcceptable,
                    .code = "not_acceptable",
                    .message = "no acceptable response content coding"});
            }
            state_.emplace<Identity>();
            return;
        }
        state_.emplace<Pending>();
    }

    // Binds encoder ownership to the protocol body plan. Body-suppressed
    // responses retain their selected representation metadata but never create
    // an encoder for bytes that the protocol will not send.
    void activate(HttpResponseBodyPlan bodyPlan) {
        if (std::holds_alternative<Identity>(state_) ||
            std::holds_alternative<Suppressed>(state_)) {
            return;
        }
        const auto* pending = std::get_if<Pending>(&state_);
        if (pending == nullptr) {
            throw std::logic_error("streaming response compression is not pending");
        }
        if (bodyPlan.bodySuppressed()) {
            state_.emplace<Suppressed>();
            return;
        }
        state_.emplace<Active>(selection_.coding(), encodedChunk_.get_allocator().resource());
    }

    [[nodiscard]] bool active() const noexcept {
        return std::holds_alternative<Active>(state_);
    }

    // A stream-head transaction may prepare representation metadata before the
    // protocol layer accepts the head. If that later preparation fails, the
    // sink is terminal before any body byte can be retried; keep the encoder
    // lifecycle in the same terminal state instead of leaving Pending/Identity
    // behind for a second commit attempt.
    void abort() noexcept {
        encodedChunk_.clear();
        state_.emplace<Failed>();
    }

    [[nodiscard]] HttpContentEncodeStep write(std::string_view input) {
        if (std::holds_alternative<Failed>(state_)) {
            encodedChunk_.clear();
            return HttpContentEncodeStep::kFailure;
        }
        if (std::holds_alternative<Finished>(state_)) {
            encodedChunk_.clear();
            return HttpContentEncodeStep::kFailure;
        }
        auto* activeState = std::get_if<Active>(&state_);
        if (activeState == nullptr) {
            throw std::logic_error("streaming response encoder is not active");
        }
        encodedChunk_.clear();
        const auto result = activeState->encoder.write(input, encodedChunk_, true);
        if (result == HttpContentEncodeStep::kFailure) {
            state_.emplace<Failed>();
        }
        return result;
    }

    [[nodiscard]] HttpContentEncodeStep finish() {
        if (std::holds_alternative<Failed>(state_)) {
            encodedChunk_.clear();
            return HttpContentEncodeStep::kFailure;
        }
        if (std::holds_alternative<Finished>(state_)) {
            encodedChunk_.clear();
            return HttpContentEncodeStep::kFinished;
        }
        auto* activeState = std::get_if<Active>(&state_);
        if (activeState == nullptr) {
            throw std::logic_error("streaming response encoder is not active");
        }
        encodedChunk_.clear();
        const auto result = activeState->encoder.finish(encodedChunk_);
        if (result == HttpContentEncodeStep::kFinished) {
            state_.emplace<Finished>();
        } else if (result == HttpContentEncodeStep::kFailure) {
            state_.emplace<Failed>();
        }
        return result;
    }

    [[nodiscard]] std::string_view output() const& noexcept {
        return encodedChunk_;
    }
    std::string_view output() const&& = delete;

private:
    struct Unprepared final {};
    struct Identity final {};
    struct Pending final {};
    struct Suppressed final {};
    struct Finished final {};
    struct Failed final {};

    struct Active final {
        Active(HttpContentCoding coding, std::pmr::memory_resource* resource)
            : encoder(coding, resource) {}

        HttpContentEncoder encoder;
    };

    using State = std::variant<Unprepared, Identity, Pending, Suppressed, Finished, Failed, Active>;

    HttpResponseCodingSelection selection_;
    HttpResponseCodingAvailability availability_;
    std::pmr::string encodedChunk_;
    State state_;
};

}  // namespace ruvia::detail
