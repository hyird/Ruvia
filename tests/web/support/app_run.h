#pragma once

#include <exception>
#include <mutex>
#include <thread>

#include "ruvia/web/app.h"

namespace ruvia::test {

// Startup hooks remain caller-owned: application retains them across runs.
class app_run final {
public:
    explicit app_run(application& app)
        : app_(app),
          thread_([this] {
              try {
                  app_.run();
              } catch (...) {
                  std::lock_guard lock(mutex_);
                  failure_ = std::current_exception();
              }
          }) {}

    app_run(const app_run&) = delete;
    app_run& operator=(const app_run&) = delete;

    ~app_run() {
        if (thread_.joinable()) {
            app_.stop();
            thread_.join();
        }
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (const auto error = failure()) {
            std::rethrow_exception(error);
        }
    }

    [[nodiscard]] std::exception_ptr failure() const {
        std::lock_guard lock(mutex_);
        return failure_;
    }

    [[nodiscard]] std::thread::id id() const noexcept {
        return thread_.get_id();
    }

private:
    application& app_;
    mutable std::mutex mutex_;
    std::exception_ptr failure_;
    std::thread thread_;
};

}  // namespace ruvia::test
