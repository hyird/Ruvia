#include "ruvia/edge/EdgeConfig.h"

#include <utility>

namespace ruvia::edge {

const OriginSettings* EdgeConfigSnapshot::findOrigin(
    std::string_view frontHost) const noexcept {
    const auto it = origins_.find(frontHost);
    return it != origins_.end() ? &it->second : nullptr;
}

EdgeConfig::EdgeConfig()
    : current_(std::make_shared<const EdgeConfigSnapshot>()) {}

std::shared_ptr<const EdgeConfigSnapshot> EdgeConfig::snapshot() const noexcept {
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
}

bool EdgeConfig::addOrigin(std::string frontHost, OriginSettings settings) {
    const std::lock_guard<std::mutex> lock(writeMutex_);
    const auto previous =
        std::atomic_load_explicit(&current_, std::memory_order_relaxed);
    auto next = std::make_shared<EdgeConfigSnapshot>(*previous);
    const auto inserted =
        next->origins_.insert_or_assign(
            std::move(frontHost), std::move(settings)).second;
    std::atomic_store_explicit(
        &current_,
        std::shared_ptr<const EdgeConfigSnapshot>(std::move(next)),
        std::memory_order_release);
    return inserted;
}

bool EdgeConfig::removeOrigin(std::string_view frontHost) {
    const std::lock_guard<std::mutex> lock(writeMutex_);
    const auto previous =
        std::atomic_load_explicit(&current_, std::memory_order_relaxed);
    const auto it = previous->origins_.find(frontHost);
    if (it == previous->origins_.end()) {
        return false;
    }
    auto next = std::make_shared<EdgeConfigSnapshot>(*previous);
    next->origins_.erase(next->origins_.find(frontHost));
    std::atomic_store_explicit(
        &current_,
        std::shared_ptr<const EdgeConfigSnapshot>(std::move(next)),
        std::memory_order_release);
    return true;
}

}  // namespace ruvia::edge
