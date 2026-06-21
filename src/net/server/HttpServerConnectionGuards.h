#pragma once

#include "HttpConnectionState.h"

#include <cstddef>

namespace ruvia::detail {

class ConnectionCountGuard final {
public:
    explicit ConnectionCountGuard(std::size_t& count) noexcept
        : count_(&count) {}

    ConnectionCountGuard(const ConnectionCountGuard&) = delete;
    ConnectionCountGuard& operator=(const ConnectionCountGuard&) = delete;

    ~ConnectionCountGuard() {
        if (count_ != nullptr && *count_ > 0) {
            --*count_;
        }
    }

private:
    std::size_t* count_;
};

// Returns a connection's borrowed work set to the per-worker pool on scope exit.
// Tracks the pointer variable by reference so explicit idle-gap releases are not
// double-released.
class WorkSetReturn final {
public:
    WorkSetReturn(ConnectionWorkSetPool& pool, ConnectionWorkSet*& workSet) noexcept
        : pool_(&pool), workSet_(&workSet) {}

    WorkSetReturn(const WorkSetReturn&) = delete;
    WorkSetReturn& operator=(const WorkSetReturn&) = delete;

    ~WorkSetReturn() {
        if (*workSet_ != nullptr) {
            pool_->release(*workSet_);
        }
    }

private:
    ConnectionWorkSetPool* pool_;
    ConnectionWorkSet** workSet_;
};

}  // namespace ruvia::detail
