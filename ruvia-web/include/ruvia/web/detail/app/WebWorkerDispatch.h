#pragma once

#include <asio/any_io_executor.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stop_token>

#include "ruvia/web/WebWorker.h"

namespace ruvia::detail {

class DbRegistry;
class RedisRegistry;

class WebWorkerDispatch final
    : public std::enable_shared_from_this<WebWorkerDispatch> {
public:
    using Task = std::move_only_function<ruvia::Task<void>(WebWorkerContext&)>;

    WebWorkerDispatch(
        asio::any_io_executor executor,
        WorkerHandle worker,
        std::pmr::memory_resource* resource,
        DbRegistry& databases,
        RedisRegistry& redis,
        std::move_only_function<void()> drained,
        std::move_only_function<void(std::exception_ptr)> failed);
    ~WebWorkerDispatch();

    WebWorkerDispatch(const WebWorkerDispatch&) = delete;
    WebWorkerDispatch& operator=(const WebWorkerDispatch&) = delete;

    [[nodiscard]] WebWorkerHandle handle();
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] WorkerId id() const noexcept;
    [[nodiscard]] PostResult post(Task task);
    void close() noexcept;
    void retire() noexcept;
    [[nodiscard]] bool accepting() const noexcept;
    [[nodiscard]] std::size_t outstanding() const noexcept;
    [[nodiscard]] WebWorkerStats stats() const noexcept;

private:
    void start(Task task);
    [[nodiscard]] ruvia::Task<void> run(Task task);
    void complete();

    asio::any_io_executor executor_;
    WorkerHandle worker_;
    std::pmr::memory_resource* resource_;
    DbRegistry* databases_;
    RedisRegistry* redis_;
    std::move_only_function<void()> drained_;
    std::move_only_function<void(std::exception_ptr)> failed_;
    mutable std::mutex submitMutex_;
    std::stop_source stopSource_;
    std::atomic_size_t outstanding_{0};
    std::atomic_uint64_t accepted_{0};
    std::atomic_uint64_t queueFull_{0};
    std::atomic_uint64_t workerStopping_{0};
    std::atomic_uint64_t completed_{0};
    std::atomic_uint64_t failedCount_{0};
    bool accepting_{true};
};

}  // namespace ruvia::detail
