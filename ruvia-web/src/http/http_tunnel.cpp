#include "ruvia/web/http_tunnel.h"

#include <stdexcept>
#include <utility>

#include "ruvia/core/bytes.h"

#include "util/operation_lane_lease.h"

namespace ruvia {
namespace {
constexpr const char* tunnel_lane_in_progress = "HTTP tunnel operation already in progress on this direction";
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
    task<std::optional<std::pmr::string>> (*read)(void*), bool& read_active, unsigned& running) {
    auto lease = detail::claim_operation_lane(read_active, tunnel_lane_in_progress);
    running_tunnel_operation active(running);
    co_return co_await read(target);
}
task<void> write_tunnel(void* target, task<void> (*write)(void*, std::string_view),
    std::pmr::string bytes_value, bool& write_active, bool& ended, unsigned& running) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
    if (ended) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    running_tunnel_operation active(running);
    co_await write(target, bytes_value);
}
task<void> finish_tunnel(void* target, task<void> (*finish_value)(void*),
    bool& ended, bool& write_active, unsigned& running) {
    auto lease = detail::claim_operation_lane(write_active, tunnel_lane_in_progress);
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
    return ::ruvia::make_scoped_operation(operations_, read_tunnel(target_, read_, read_active_, running_operations_),
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
    return ::ruvia::make_scoped_operation(operations_, write_tunnel(target_, write_, std::move(owned), write_active_, send_ended_, running_operations_),
        &check_tunnel_worker, const_cast<worker_handle*>(&worker_));
}
scoped_operation<void> http_tunnel::finish() & {
    require_active();
    return ::ruvia::make_scoped_operation(operations_, finish_tunnel(target_, finish_, send_ended_, write_active_, running_operations_),
        &check_tunnel_worker, const_cast<worker_handle*>(&worker_));
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
