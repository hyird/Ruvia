#pragma once

#include <stdexcept>
#include <utility>

#include "ruvia/core/scoped_operation.h"

#include "test_harness.h"

namespace streaming_test {

class test_scoped_capability final {
public:
    test_scoped_capability(ruvia::operation_scope& scope, int& expired_count) noexcept
        : expired_count_(&expired_count),
          registration_(scope, this, &test_scoped_capability::expire) {}

    test_scoped_capability(const test_scoped_capability& other) noexcept
        : expired_count_(other.expired_count_),
          registration_(other.registration_, this) {}

    test_scoped_capability(test_scoped_capability&& other) noexcept
        : expired_count_(std::exchange(other.expired_count_, nullptr)),
          registration_(std::move(other.registration_), this) {}

    void use() const {
        registration_.require_active();
    }

private:
    static void expire(void* target) noexcept {
        auto& capability = *static_cast<test_scoped_capability*>(target);
        ++*capability.expired_count_;
    }

    int* expired_count_;
    ruvia::scoped_capability_registration registration_;
};

struct cold_frame_probe final {
    bool* destroyed_;

    explicit cold_frame_probe(bool& destroyed) noexcept
        : destroyed_(&destroyed) {}

    cold_frame_probe(cold_frame_probe&& other) noexcept
        : destroyed_(std::exchange(other.destroyed_, nullptr)) {}

    ~cold_frame_probe() {
        if (destroyed_ != nullptr) {
            *destroyed_ = true;
        }
    }
};

inline ruvia::task<void> cold_frame_task(cold_frame_probe probe) {
    (void)probe;
    co_return;
}

}  // namespace streaming_test

using namespace streaming_test;  // NOLINT(google-build-using-namespace)
