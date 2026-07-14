#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <ruvia/core/WorkerHandle.h>

namespace ruvia {

struct RuntimeOptions {
    std::size_t workerCount{0};
    std::size_t mailboxCapacity{1024};
};

class Runtime {
public:
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    void start();
    void stop() noexcept;
    void join();

    [[nodiscard]] std::size_t workerCount() const noexcept;
    [[nodiscard]] WorkerHandle worker(std::size_t index) const;
    [[nodiscard]] WorkerHandle nextWorker() noexcept;
    [[nodiscard]] WorkerHandle workerFor(std::uint64_t key) const noexcept;
    [[nodiscard]] WorkerHandle workerFor(std::string_view key) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
