#include "http3/http3_quic_client_endpoint_resolver.h"

#include <algorithm>
#include <coroutine>
#include <exception>
#include <stdexcept>
#include <utility>

#include <asio/error.hpp>

#include "client/client_transport.h"
#include "http3/http3_quic_socket_address.h"

namespace ruvia::detail {
namespace {

std::pmr::memory_resource* checked_resource(std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        throw std::invalid_argument("QUIC DNS resolver requires a worker memory resource");
    }
    return resource;
}

// One coroutine-frame awaiter: DNS and deadline timer may race, but neither
// callback may outlive the frame. The first completion cancels its sibling;
// the coroutine resumes only after every armed handler has been delivered.
class resolve_wait final {
public:
    using resolver_type = asio::ip::udp::resolver;
    using status_type = http3_quic_client_endpoint_resolver::status_type;
    using time_point_type = http3_quic_client_endpoint_resolver::time_point_type;
    struct completion_type final {
        status_type status_{status_type::resolve_failed};
        std::error_code error_{};
        resolver_type::results_type results_{};
    };

    resolve_wait(resolver_type& resolver, asio::steady_timer& timer, bool& stopping,
        bool& active, std::uint64_t& generation, std::string_view host,
        std::string_view service, std::optional<time_point_type> deadline_value) noexcept
        : resolver_(resolver),
          timer_(timer),
          stopping_(stopping),
          active_(active),
          generation_(generation),
          started_generation_(generation),
          host_(host),
          service_(service),
          deadline_(deadline_value) {}
    resolve_wait(const resolve_wait&) = delete;
    resolve_wait& operator=(const resolve_wait&) = delete;
    ~resolve_wait() {
        if (pending_ != 0) {
            std::terminate();
        }
    }

    [[nodiscard]] bool await_ready() noexcept {
        if (stopping_) {
            status_ = status_type::stopped;
            return true;
        }
        if (active_) {
            status_ = status_type::already_resolving;
            return true;
        }
        if (deadline_ && std::chrono::steady_clock::now() >= *deadline_) {
            status_ = status_type::timeout;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        if (stopping_) {
            status_ = status_type::stopped;
            return false;
        }
        if (active_) {
            status_ = status_type::already_resolving;
            return false;
        }
        active_ = true;
        owns_active_ = true;
        continuation_ = continuation;
        arming_ = true;
        try {
            if (deadline_) {
                timer_.expires_at(*deadline_);
                ++pending_;
                try {
                    timer_.async_wait([this](const asio::error_code& error) {
                        timer_completed(error);
                    });
                } catch (...) {
                    --pending_;
                    throw;
                }
            }
            if (!winner_chosen_) {
                ++pending_;
                try {
                    resolver_.async_resolve(host_, service_,
                        [this](const asio::error_code& error, resolver_type::results_type results) {
                            resolved(error, std::move(results));
                        });
                } catch (...) {
                    --pending_;
                    throw;
                }
            }
        } catch (...) {
            failure_ = std::current_exception();
            winner_chosen_ = true;
            cancel_outstanding();
            if (pending_ == 0) {
                woke_ = true;
            }
        }
        arming_ = false;
        return !woke_;
    }

    [[nodiscard]] completion_type await_resume() {
        if (owns_active_) {
            active_ = false;
        }
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        if (stopping_) {
            return {.status_ = status_type::stopped};
        }
        if (generation_ != started_generation_) {
            return {.status_ = status_type::interrupted};
        }
        if (deadline_ && std::chrono::steady_clock::now() >= *deadline_) {
            return {.status_ = status_type::timeout};
        }
        return {status_, error_, std::move(results_)};
    }

private:
    void cancel_outstanding() noexcept {
        asio::error_code ignored;
        (void)timer_.cancel();
        resolver_.cancel();
    }

    void resolved(const asio::error_code& error, resolver_type::results_type results) {
        if (pending_ == 0) {
            std::terminate();
        }
        --pending_;
        if (!winner_chosen_) {
            winner_chosen_ = true;
            error_ = error;
            status_ = stopping_                            ? status_type::stopped
                      : generation_ != started_generation_ ? status_type::interrupted
                                                           : status_type::resolve_failed;
            if (!stopping_ && generation_ == started_generation_ && !error) {
                try {
                    results_ = std::move(results);
                    status_ = status_type::resolved;
                } catch (...) {
                    failure_ = std::current_exception();
                }
            }
            cancel_outstanding();
        }
        finish();
    }

    void timer_completed(const asio::error_code& error) {
        if (pending_ == 0) {
            std::terminate();
        }
        --pending_;
        if (!winner_chosen_) {
            winner_chosen_ = true;
            status_ = stopping_                            ? status_type::stopped
                      : generation_ != started_generation_ ? status_type::interrupted
                      : error                              ? status_type::resolve_failed
                                                           : status_type::timeout;
            error_ = error;
            cancel_outstanding();
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

    resolver_type& resolver_;
    asio::steady_timer& timer_;
    bool& stopping_;
    bool& active_;
    std::uint64_t& generation_;
    std::uint64_t started_generation_;
    std::string_view host_;
    std::string_view service_;
    std::optional<time_point_type> deadline_;
    std::coroutine_handle<> continuation_{};
    std::exception_ptr failure_;
    resolver_type::results_type results_{};
    std::error_code error_{};
    status_type status_{status_type::resolve_failed};
    std::size_t pending_{};
    bool owns_active_{};
    bool arming_{};
    bool winner_chosen_{};
    bool woke_{};
};

}  // namespace

http3_quic_client_endpoint_resolver::http3_quic_client_endpoint_resolver(asio::io_context& io,
    std::pmr::memory_resource* worker_resource)
    : owner_thread_(std::this_thread::get_id()),
      resource_(checked_resource(worker_resource)),
      resolver_(io),
      deadline_timer_(io) {}

http3_quic_client_endpoint_resolver::~http3_quic_client_endpoint_resolver() {
    if (std::this_thread::get_id() != owner_thread_ || resolving_) {
        std::terminate();
    }
    request_stop();
}

void http3_quic_client_endpoint_resolver::require_owner_thread() const {
    if (std::this_thread::get_id() != owner_thread_) {
        throw std::logic_error("QUIC DNS resolver used outside its worker");
    }
}

task<http3_quic_client_endpoint_resolver::result_type> http3_quic_client_endpoint_resolver::resolve(
    std::string_view host, std::uint16_t port, std::optional<time_point_type> absolute_deadline) {
    require_owner_thread();
    if (port == 0) {
        throw std::invalid_argument("QUIC DNS requires a nonzero UDP port");
    }
    validate_client_origin_host(host, "QUIC DNS requires a hostname",
        "QUIC DNS hostname is invalid");
    std::pmr::string owned(host, resource_);
    return resolve_owned(std::move(owned), port, absolute_deadline);
}

task<http3_quic_client_endpoint_resolver::result_type> http3_quic_client_endpoint_resolver::resolve_owned(
    std::pmr::string host, std::uint16_t port, std::optional<time_point_type> absolute_deadline) {
    require_owner_thread();
    result_type result(resource_);
    client_port_text_buffer_type port_buffer{};
    const auto service = format_client_port(port, port_buffer);
    auto outcome = co_await resolve_wait(resolver_, deadline_timer_, stopping_, resolving_,
        generation_, host, service, absolute_deadline);
    result.status_ = outcome.status_;
    result.error_ = outcome.error_;
    if (result.status_ == status_type::resolved) {
        for (const auto& entry : outcome.results_) {
            const auto endpoint = entry.endpoint();
            if ((to_http3_quic_datagram_address(endpoint).index() != 0) ||
                std::find(result.endpoints_.begin(), result.endpoints_.end(), endpoint) !=
                    result.endpoints_.end()) {
                continue;
            }
            result.endpoints_.push_back(endpoint);
            if (result.endpoints_.size() == max_endpoints) {
                break;
            }
        }
        if (result.endpoints_.empty()) {
            result.status_ = status_type::no_supported_address;
        }
    }
    co_return result;
}

void http3_quic_client_endpoint_resolver::request_stop() noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
    stopping_ = true;
    resolver_.cancel();
    asio::error_code ignored;
    (void)deadline_timer_.cancel();
}

void http3_quic_client_endpoint_resolver::interrupt() noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
    if (!resolving_ || stopping_) {
        return;
    }
    ++generation_;
    resolver_.cancel();
    asio::error_code ignored;
    (void)deadline_timer_.cancel();
}

}  // namespace ruvia::detail
