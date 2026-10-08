#include "ruvia/web/HttpTunnel.h"

#include <stdexcept>
#include <utility>

#include "ruvia/core/Bytes.h"

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
class RunningTunnelOperation final {
public:
    explicit RunningTunnelOperation(unsigned& count) noexcept
        : count_(count) {
        ++count_;
    }
    ~RunningTunnelOperation() {
        --count_;
    }

private:
    unsigned& count_;
};
void checkTunnelWorker(void* raw) noexcept {
    if (!static_cast<const WorkerHandle*>(raw)->isCurrent()) {
        std::terminate();
    }
}
Task<std::optional<std::pmr::string>> readTunnel(void* target,
    Task<std::optional<std::pmr::string>> (*read)(void*), detail::operation_lane_lease lease, unsigned& running) {
    static_cast<void>(lease);
    RunningTunnelOperation active(running);
    co_return co_await read(target);
}
Task<void> writeTunnel(void* target, Task<void> (*write)(void*, std::string_view),
    std::pmr::string bytes, detail::operation_lane_lease lease, unsigned& running) {
    static_cast<void>(lease);
    RunningTunnelOperation active(running);
    co_await write(target, bytes);
}
Task<void> finishTunnel(void* target, Task<void> (*finish)(void*),
    bool& ended, detail::operation_lane_lease lease, unsigned& running) {
    static_cast<void>(lease);
    RunningTunnelOperation active(running);
    if (!ended) {
        co_await finish(target);
        ended = true;
    }
}
}  // namespace

HttpTunnel::HttpTunnel(std::pmr::memory_resource& resource, const WorkerHandle& worker,
    void* target, Read read, Write write, Finish finish, Abort abort) noexcept
    : resource_(resource),
      worker_(worker),
      target_(target),
      read_(read),
      write_(write),
      finish_(finish),
      abort_(abort) {}

void HttpTunnel::requireActive() const {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    if (!operations_.active()) {
        throw std::logic_error("HTTP tunnel session has expired");
    }
}
ScopedOperation<std::optional<std::pmr::string>> HttpTunnel::read() & {
    requireActive();
    return ::ruvia::make_scoped_operation(operations_, readTunnel(target_, read_, claim_tunnel_lane(readActive_), runningOperations_),
        &checkTunnelWorker, const_cast<WorkerHandle*>(&worker_));
}
ScopedOperation<void> HttpTunnel::write(std::string_view bytes) & {
    requireActive();
    return write(std::pmr::string(bytes, &resource_));
}
ScopedOperation<void> HttpTunnel::write(std::span<const std::byte> bytes) & {
    return write(asChars(bytes));
}
ScopedOperation<void> HttpTunnel::write(std::pmr::string&& bytes) & {
    requireActive();
    if (sendEnded_) {
        throw std::logic_error("HTTP tunnel send direction has ended");
    }
    std::pmr::string owned(std::move(bytes), &resource_);
    return ::ruvia::make_scoped_operation(operations_, writeTunnel(target_, write_, std::move(owned), claim_tunnel_lane(writeActive_), runningOperations_),
        &checkTunnelWorker, const_cast<WorkerHandle*>(&worker_));
}
ScopedOperation<void> HttpTunnel::finish() & {
    requireActive();
    return ::ruvia::make_scoped_operation(operations_, finishTunnel(target_, finish_, sendEnded_, claim_tunnel_lane(writeActive_), runningOperations_),
        &checkTunnelWorker, const_cast<WorkerHandle*>(&worker_));
}
ScopedOperation<std::optional<detail::HttpDatagramInput>> HttpTunnel::readDatagramInput() {
    requireActive();
    const auto read = +[](void* target, ReadDatagramInput fn, detail::operation_lane_lease lease, unsigned& count) -> Task<std::optional<detail::HttpDatagramInput>> {
        static_cast<void>(lease);
        RunningTunnelOperation running(count);
        co_return co_await fn(target);
    };
    return ::ruvia::make_scoped_operation(operations_, read(target_, readDatagramInput_, claim_tunnel_lane(readActive_), runningOperations_), &checkTunnelWorker, const_cast<WorkerHandle*>(&worker_));
}
ScopedOperation<void> HttpTunnel::sendDatagram(std::string_view bytes) {
    requireActive();
    if (sendEnded_ || !sendDatagram_) {
        throw std::logic_error("HTTP Datagram sending direction is closed");
    }
    const auto send = +[](void* target, SendDatagram fn, std::pmr::string owned, detail::operation_lane_lease lease, unsigned& count) -> Task<void> {
        static_cast<void>(lease);
        RunningTunnelOperation running(count);
        fn(target, std::as_bytes(std::span(owned.data(), owned.size())));
        co_return;
    };
    return ::ruvia::make_scoped_operation(operations_, send(target_, sendDatagram_, std::pmr::string(bytes, &resource_), claim_tunnel_lane(writeActive_), runningOperations_), &checkTunnelWorker, const_cast<WorkerHandle*>(&worker_));
}
void HttpTunnel::abort() noexcept {
    if (!worker_.isCurrent()) {
        std::terminate();
    }
    if (operations_.active()) {
        abort_(target_);
    }
}
}  // namespace ruvia
