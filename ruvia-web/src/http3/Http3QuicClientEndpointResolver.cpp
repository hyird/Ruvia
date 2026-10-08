#include "http3/Http3QuicClientEndpointResolver.h"

#include <algorithm>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>

#include <asio/error.hpp>

#include "client/ClientTransport.h"
#include "http3/Http3QuicSocketAddress.h"

namespace ruvia::detail {
namespace {

std::pmr::memory_resource* checkedResource(std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        throw std::invalid_argument("QUIC DNS resolver requires a worker memory resource");
    }
    return resource;
}

// One coroutine-frame awaiter: DNS and deadline timer may race, but neither
// callback may outlive the frame. The first completion cancels its sibling;
// the coroutine resumes only after every armed handler has been delivered.
class ResolveWait final {
public:
    using Resolver = asio::ip::udp::resolver;
    using Status = Http3QuicClientEndpointResolver::Status;
    using TimePoint = Http3QuicClientEndpointResolver::TimePoint;
    struct Completion final {
        Status status{Status::kResolveFailed};
        std::error_code error{};
        Resolver::results_type results{};
    };

    ResolveWait(Resolver& resolver, asio::steady_timer& timer, bool& stopping,
        bool& active, std::uint64_t& generation, std::string_view host,
        std::string_view service, std::optional<TimePoint> deadline) noexcept
        : resolver_(resolver),
          timer_(timer),
          stopping_(stopping),
          active_(active),
          generation_(generation),
          startedGeneration_(generation),
          host_(host),
          service_(service),
          deadline_(deadline) {}
    ResolveWait(const ResolveWait&) = delete;
    ResolveWait& operator=(const ResolveWait&) = delete;
    ~ResolveWait() {
        if (pending_ != 0) {
            std::terminate();
        }
    }

    [[nodiscard]] bool await_ready() noexcept {
        if (stopping_) {
            status_ = Status::kStopped;
            return true;
        }
        if (active_) {
            status_ = Status::kAlreadyResolving;
            return true;
        }
        if (deadline_ && std::chrono::steady_clock::now() >= *deadline_) {
            status_ = Status::kTimeout;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        if (stopping_) {
            status_ = Status::kStopped;
            return false;
        }
        if (active_) {
            status_ = Status::kAlreadyResolving;
            return false;
        }
        active_ = true;
        ownsActive_ = true;
        continuation_ = continuation;
        arming_ = true;
        try {
            if (deadline_) {
                timer_.expires_at(*deadline_);
                ++pending_;
                try {
                    timer_.async_wait([this](const asio::error_code& error) {
                        timerCompleted(error);
                    });
                } catch (...) {
                    --pending_;
                    throw;
                }
            }
            if (!winnerChosen_) {
                ++pending_;
                try {
                    resolver_.async_resolve(host_, service_,
                        [this](const asio::error_code& error, Resolver::results_type results) {
                            resolved(error, std::move(results));
                        });
                } catch (...) {
                    --pending_;
                    throw;
                }
            }
        } catch (...) {
            failure_ = std::current_exception();
            winnerChosen_ = true;
            cancelOutstanding();
            if (pending_ == 0) {
                woke_ = true;
            }
        }
        arming_ = false;
        return !woke_;
    }

    [[nodiscard]] Completion await_resume() {
        if (ownsActive_) {
            active_ = false;
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (stopping_) {
            return {.status = Status::kStopped};
        }
        if (generation_ != startedGeneration_) {
            return {.status = Status::kInterrupted};
        }
        if (deadline_ && std::chrono::steady_clock::now() >= *deadline_) {
            return {.status = Status::kTimeout};
        }
        return {status_, error_, std::move(results_)};
    }

private:
    void cancelOutstanding() noexcept {
        asio::error_code ignored;
        (void)timer_.cancel(ignored);
        resolver_.cancel();
    }

    void resolved(const asio::error_code& error, Resolver::results_type results) {
        if (pending_ == 0) {
            std::terminate();
        }
        --pending_;
        if (!winnerChosen_) {
            winnerChosen_ = true;
            error_ = error;
            status_ = stopping_                           ? Status::kStopped
                      : generation_ != startedGeneration_ ? Status::kInterrupted
                                                          : Status::kResolveFailed;
            if (!stopping_ && generation_ == startedGeneration_ && !error) {
                try {
                    results_ = std::move(results);
                    status_ = Status::kResolved;
                } catch (...) {
                    failure_ = std::current_exception();
                }
            }
            cancelOutstanding();
        }
        finish();
    }

    void timerCompleted(const asio::error_code& error) {
        if (pending_ == 0) {
            std::terminate();
        }
        --pending_;
        if (!winnerChosen_) {
            winnerChosen_ = true;
            status_ = stopping_                           ? Status::kStopped
                      : generation_ != startedGeneration_ ? Status::kInterrupted
                      : error                             ? Status::kResolveFailed
                                                          : Status::kTimeout;
            error_ = error;
            cancelOutstanding();
        }
        finish();
    }

    void finish() {
        if (pending_ != 0 || woke_) {
            return;
        }
        woke_ = true;
        if (!arming_) {
            continuation_.resume();
        }
    }

    Resolver& resolver_;
    asio::steady_timer& timer_;
    bool& stopping_;
    bool& active_;
    std::uint64_t& generation_;
    std::uint64_t startedGeneration_;
    std::string_view host_;
    std::string_view service_;
    std::optional<TimePoint> deadline_;
    std::coroutine_handle<> continuation_{};
    std::exception_ptr failure_;
    Resolver::results_type results_{};
    std::error_code error_{};
    Status status_{Status::kResolveFailed};
    std::size_t pending_{};
    bool ownsActive_{};
    bool arming_{};
    bool winnerChosen_{};
    bool woke_{};
};

}  // namespace

Http3QuicClientEndpointResolver::Http3QuicClientEndpointResolver(asio::io_context& io,
    std::pmr::memory_resource* workerResource)
    : ownerThread_(std::this_thread::get_id()),
      resource_(checkedResource(workerResource)),
      resolver_(io),
      deadlineTimer_(io) {}

Http3QuicClientEndpointResolver::~Http3QuicClientEndpointResolver() {
    if (std::this_thread::get_id() != ownerThread_ || resolving_) {
        std::terminate();
    }
    requestStop();
}

void Http3QuicClientEndpointResolver::requireOwnerThread() const {
    if (std::this_thread::get_id() != ownerThread_) {
        throw std::logic_error("QUIC DNS resolver used outside its worker");
    }
}

Task<Http3QuicClientEndpointResolver::Result> Http3QuicClientEndpointResolver::resolve(
    std::string_view host, std::uint16_t port, std::optional<TimePoint> absoluteDeadline) {
    requireOwnerThread();
    if (port == 0) {
        throw std::invalid_argument("QUIC DNS requires a nonzero UDP port");
    }
    validateClientOriginHost(host, "QUIC DNS requires a hostname",
        "QUIC DNS hostname is invalid");
    std::pmr::string owned(host, resource_);
    return resolveOwned(std::move(owned), port, absoluteDeadline);
}

Task<Http3QuicClientEndpointResolver::Result> Http3QuicClientEndpointResolver::resolveOwned(
    std::pmr::string host, std::uint16_t port, std::optional<TimePoint> absoluteDeadline) {
    requireOwnerThread();
    Result result(resource_);
    ClientPortTextBuffer portBuffer{};
    const auto service = formatClientPort(port, portBuffer);
    auto outcome = co_await ResolveWait(resolver_, deadlineTimer_, stopping_, resolving_,
        generation_, host, service, absoluteDeadline);
    result.status = outcome.status;
    result.error = outcome.error;
    if (result.status == Status::kResolved) {
        for (const auto& entry : outcome.results) {
            const auto endpoint = entry.endpoint();
            if (!to_http3_quic_datagram_address(endpoint) ||
                std::find(result.endpoints.begin(), result.endpoints.end(), endpoint) !=
                    result.endpoints.end()) {
                continue;
            }
            result.endpoints.push_back(endpoint);
            if (result.endpoints.size() == kMaxEndpoints) {
                break;
            }
        }
        if (result.endpoints.empty()) {
            result.status = Status::kNoSupportedAddress;
        }
    }
    co_return result;
}

void Http3QuicClientEndpointResolver::requestStop() noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    stopping_ = true;
    resolver_.cancel();
    asio::error_code ignored;
    (void)deadlineTimer_.cancel(ignored);
}

void Http3QuicClientEndpointResolver::interrupt() noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
    if (!resolving_ || stopping_) {
        return;
    }
    ++generation_;
    resolver_.cancel();
    asio::error_code ignored;
    (void)deadlineTimer_.cancel(ignored);
}

}  // namespace ruvia::detail
