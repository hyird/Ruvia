#include "ruvia/web/http_tunnel.h"

#include <stdexcept>
#include <utility>

#include "ruvia/core/bytes.h"

#include "util/operation_lane_lease.h"

namespace ruvia {
namespace {
detail::operation_lane_lease claim_tunnel_lane(bool& active) {
    detail::operation_lane_lease lease(active);
    if (!lease) {
        throw std::logic_error("HTTP tunnel operation already in progress on this direction");
    }
    return lease;
}
class running_tunnel_operation final {
public:
    explicit running_tunnel_operation(unsigned& count) noexcept
        : count_(count) {
        ++count_;
    }
    ~running_tunnel_operation() {
        --count_;
    }

private:
    unsigned& count_;
};
void check_tunnel_worker(void* raw) noexcept {
    if (!static_cast<const worker_handle*>(raw)->is_current()) {
        std::terminate();
    }
}
task<std::optional<std::pmr::string>> read_tunnel(void* target,
    task<std::optional<std::pmr::string>> (*read)(void*), detail::operation_lane_lease lease_value, unsigned& running) {
    static_cast<void>(lease_value);
    running_tunnel_operation active(running);
    co_return co_await read(target);
}
task<void> write_tunnel(void* target, task<void> (*write)(void*, std::string_view),
    std::pmr::string bytes_value, detail::operation_lane_lease lease_value, unsigned& running) {
    static_cast<void>(lease_value);
    running_tunnel_operation active(running);
    co_await write(target, bytes_value);
}
task<void> finish_tunnel(void* target, task<void> (*finish_value)(void*),
    bool& ended, detail::operation_lane_lease lease_value, unsigned& running) {
    static_cast<void>(lease_value);
    running_tunnel_operation active(running);
    if (!ended) {
        co_await finish_value(target);
        ended = true;
    }
}
}  // namespace

http_tunnel::http_tunnel(std::pmr::memory_resource& resource, const worker_handle& worker_value,
    void* target, read_type read, write_type write, finish_type finish_value, abort_type abort) noexcept
    : resource_(resource),
      worker_(worker_value),
      target_(target),
      read_(read),
      write_(write),
      finish_(finish_value),
      abort_(abort) {}

void http_tunnel::require_active() const {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (!operations_.active()) {
        throw std::logic_error("HTTP tunnel session has expired");
    }
}
scoped_operation<std::optional<std::pmr::string>> http_tunnel::read() & {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, read_tunnel(target_, read_, claim_tunnel_lane(read_active_), running_operations_),
        &check_tunnel_worker, const_cast<worker_handle*>(&worker_));
}
scoped_operation<void> http_tunnel::write(std::string_view bytes_value) & {
    require_active();
    return write(std::pmr::string(bytes_value, &resource_));
}
scoped_operation<void> http_tunnel::write(std::span<const std::byte> bytes_value) & {
    return write(as_chars(bytes_value));
}
scoped_operation<void> http_tunnel::write(std::pmr::string&& bytes_value) & {
    require_active();
    if (send_ended_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    std::pmr::string owned(std::move(bytes_value), &resource_);
    return ::ruvia::make_scoped_operation(operations_, write_tunnel(target_, write_, std::move(owned), claim_tunnel_lane(write_active_), running_operations_),
        &check_tunnel_worker, const_cast<worker_handle*>(&worker_));
}
scoped_operation<void> http_tunnel::finish() & {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, finish_tunnel(target_, finish_, send_ended_, claim_tunnel_lane(write_active_), running_operations_),
        &check_tunnel_worker, const_cast<worker_handle*>(&worker_));
}
scoped_operation<std::optional<detail::http_datagram_input>> http_tunnel::read_datagram_input() {
    require_active();
    const auto read = +[](void* target, read_datagram_input_type fn, detail::operation_lane_lease lease_value, unsigned& count) -> task<std::optional<detail::http_datagram_input>> {
        static_cast<void>(lease_value);
        running_tunnel_operation running(count);
        co_return co_await fn(target);
    };
    return ::ruvia::make_scoped_operation(operations_, read(target_, read_datagram_input_, claim_tunnel_lane(read_active_), running_operations_), &check_tunnel_worker, const_cast<worker_handle*>(&worker_));
}
scoped_operation<void> http_tunnel::send_datagram(std::string_view bytes_value) {
    require_active();
    if (send_ended_ || !send_datagram_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto send = +[](void* target, send_datagram_type fn, std::pmr::string owned, detail::operation_lane_lease lease_value, unsigned& count) -> task<void> {
        static_cast<void>(lease_value);
        running_tunnel_operation running(count);
        fn(target, std::as_bytes(std::span(owned.data(), owned.size())));
        co_return;
    };
    return ::ruvia::make_scoped_operation(operations_, send(target_, send_datagram_, std::pmr::string(bytes_value, &resource_), claim_tunnel_lane(write_active_), running_operations_), &check_tunnel_worker, const_cast<worker_handle*>(&worker_));
}
void http_tunnel::abort() noexcept {
    if (!worker_.is_current()) {
        std::terminate();
    }
    if (operations_.active()) {
        abort_(target_);
    }
}
}  // namespace ruvia
