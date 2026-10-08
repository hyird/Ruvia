#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>

#include "ruvia/http/HttpResponse.h"
#include "ruvia/web/HttpClientTypes.h"

#include "client/HttpClientResponseState.h"
#include "http3/Http3ClientBodyBudget.h"
#include "http3/Http3ClientReceiveDriver.h"

namespace ruvia::detail {

// Non-owning bridge from synchronous HTTP/3 parser events to one stable client
// response. The adapter and borrowed HttpClientResponseState must stay at fixed
// addresses on their owning worker until the QUIC stream can no longer deliver
// events. The callback copies all event views before returning and never writes
// to state.buffered, whose storage may back a view already returned to a reader.
//
// The connection owner must not retire HTTP/3 parser state from a callback.
// After drive() returns, it owns any required QUIC STOP_SENDING / stream
// termination, must guarantee that no more stream bytes can arrive, and only
// then may retire the parser and commit a terminal result here. Engine parser
// retirement alone does not stop QUIC delivery. The owner must join any driver
// that can use the adapter before destroying either borrowed object.
//
// readAllowance() bounds only bytes retained in pending + buffered. It does
// not bound allocation capacity, QUIC/OpenSSL buffering or QPACK parser state.
// collectAll uses bufferedLimit as the producer's unread-body limit; the
// public readAll(maxBytes) keeps its separate final result-size check. At the
// producer limit, bounded wire probes still allow trailers and FIN to arrive.
class Http3ClientResponseDelivery final {
public:
    enum class ReadStatus : std::uint8_t {
        kReady,
        kBackpressured,
        kRetirementRequired,
        kTerminal,
    };

    struct ReadAllowance final {
        ReadStatus status{ReadStatus::kBackpressured};
        std::size_t bytes{};
    };

    enum class RetirementReason : std::uint8_t {
        kNone,
        kResponseTooLarge,
        kProtocolError,
        kCallbackFailure,
    };

    enum class CommitStatus : std::uint8_t {
        kPending,
        kCommitted,
        kRetirementRequired,
        kAlreadyCommitted,
    };

    explicit Http3ClientResponseDelivery(HttpClientResponseState& state,
        Http3ClientBodyBudget* bodyBudget = nullptr)
        : state_(state) {
        if (bodyBudget != nullptr && !state_.bindHttp3BodyBudget(*bodyBudget)) {
            throw std::length_error("HTTP/3 response body budget is exhausted");
        }
    }
    Http3ClientResponseDelivery(const Http3ClientResponseDelivery&) = delete;
    Http3ClientResponseDelivery& operator=(const Http3ClientResponseDelivery&) = delete;
    Http3ClientResponseDelivery(Http3ClientResponseDelivery&&) = delete;
    Http3ClientResponseDelivery& operator=(Http3ClientResponseDelivery&&) = delete;

    [[nodiscard]] Http3ClientResponseEventSink eventSink() noexcept;
    [[nodiscard]] ReadAllowance readAllowance(std::size_t maxInputBytes) const noexcept;
    [[nodiscard]] RetirementReason retirementReason() const noexcept {
        return retirementReason_;
    }
    [[nodiscard]] std::exception_ptr callbackFailure() const noexcept {
        return callbackFailure_;
    }
    [[nodiscard]] std::size_t retainedBodyBytes() const noexcept {
        return state_.producerBodyBytes();
    }
    void reconcileBodyBytes() noexcept;
    // Owns the protocol-computed final-head classification by value, so it
    // remains usable after the parser has been retired.
    [[nodiscard]] std::optional<HttpResponseBodyPlan> responseBodyPlan() const noexcept {
        return responseBodyPlan_;
    }

    // Call only after drive() returned and the owner completed any required
    // QUIC/parser retirement. Nonterminal driver results leave the response
    // pending. A pending retirement request must instead be committed with
    // commitRetirementFailure() after the stream is stopped and retired.
    [[nodiscard]] CommitStatus commit(const Http3ClientReceiveDriver::Result& result) noexcept;
    [[nodiscard]] CommitStatus commitComplete() noexcept;
    [[nodiscard]] bool commitTerminalError(HttpClientError::Code error) noexcept;

    // The caller must first stop QUIC delivery and retire the HTTP/3 parser for
    // this stream. Use kResponseTooLarge for an ordinary retention overflow.
    [[nodiscard]] bool commitRetirementFailure(HttpClientError::Code error) noexcept;

    // Used after drive() has unwound (including a callback allocation failure)
    // and the outer owner has completed its connection/stream teardown.
    [[nodiscard]] bool commitFailure(std::exception_ptr failure) noexcept;

private:
    static void onEvent(void* context, const Http3ConnectionEvent& event);
    void deliver(const Http3ConnectionEvent& event);
    void commitError(HttpClientError::Code error) noexcept;
    void commitTerminal() noexcept;
    void requestRetirement(RetirementReason reason) noexcept;
    [[nodiscard]] std::size_t remainingBodyCapacity() const noexcept;

    HttpClientResponseState& state_;
    std::optional<HttpResponseBodyPlan> responseBodyPlan_{};
    std::exception_ptr callbackFailure_{};
    RetirementReason retirementReason_{RetirementReason::kNone};
};

}  // namespace ruvia::detail
