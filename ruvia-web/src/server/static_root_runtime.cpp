#include "server/static_root_runtime.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>

#include "ruvia/core/BlockingPool.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/memory/ProcessResource.h"

#include "http/StaticRootIndex.h"
#include "server/HttpServerOptions.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] StaticRootPrecompressionOptions precompression_options(
    const HttpServerOptions& options) noexcept {
    const auto* configured = options.documentRoot.precompressionOptions();
    if (configured == nullptr || !options.compression.has_value()) {
        return {};
    }
    return *configured;
}

}  // namespace

static_root_runtime::static_root_runtime(HttpServerOptions& options, const WorkerHandle& worker,
    const HttpServerWorkerState& state, std::pmr::memory_resource* resource)
    : options_(options),
      worker_(worker),
      state_(state),
      current_(nullptr, PmrObjectDeleter<StaticRoot>{processResource()}),
      retired_(resource) {
    if (options_.documentRoot.refreshOptions() != nullptr) {
        const auto* configuredRoot = options_.documentRoot.root();
        if (configuredRoot == nullptr) {
            std::terminate();
        }
        current_ = StaticRootAccess::clone(processResource(), *configuredRoot);
        const auto precompression = precompression_options(options_);
        if (precompression.enabled()) {
            StaticRootAccess::installPrecompressedVariants(
                *current_, configuredRoot, precompression);
        }
        options_.documentRoot.publish(*current_);
    }
}

static_root_runtime::~static_root_runtime() = default;

Task<void> static_root_runtime::refresh() {
    const auto* refreshOptions = options_.documentRoot.refreshOptions();
    if (refreshOptions == nullptr) {
        std::terminate();
    }
    const auto interval = refreshOptions->refreshInterval;
    const auto reclaimRetiredRoots = [this]() noexcept {
        std::erase_if(retired_, [](const snapshot_owner& root) {
            return root == nullptr || !StaticRootAccess::hasActiveBindings(*root);
        });
    };
    for (;;) {
        if (!httpServerWorkerRunning(state_)) {
            co_return;
        }
        if (co_await sleepFor(worker_, interval) ==
            TimerSleepResult::kStopRequested) {
            co_return;
        }
        if (!httpServerWorkerRunning(state_)) {
            co_return;
        }

        // Lease counts belong to the snapshot they protect. Reclaim old
        // generations independently; a long request on an older generation
        // must not pin every newer generation published by polling.
        reclaimRetiredRoots();

        const auto* currentRoot = options_.documentRoot.root();
        if (currentRoot == nullptr) {
            // The validated document-root configuration owns this invariant. Keep
            // the loop defensive anyway: a broken runtime binding must not
            // turn a background task into a null dereference on the worker.
            failures_.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }

        std::filesystem::path rootPath;
        std::optional<StaticRootConfigStorage> rootConfig;
        try {
            // Both operations copy PMR-backed configuration. They are outside
            // tryRunBlocking because the source snapshot is worker-owned, but a
            // transient allocation failure here is still a refresh failure,
            // not a reason to terminate the listener and discard its last
            // complete index.
            rootPath = currentRoot->path();
            rootConfig.emplace(StaticRootAccess::copyConfig(*currentRoot, processResource()));
        } catch (...) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        snapshot_owner candidate(nullptr, PmrObjectDeleter<StaticRoot>{processResource()});
        try {
            auto rebuilt = co_await ruvia::tryRunBlocking(*options_.blockingPool,
                worker_,
                [rootPath = std::move(rootPath), rootConfig = std::move(*rootConfig)]() mutable {
                    return StaticRootAccess::make(
                        processResource(), rootPath, std::move(rootConfig));
                });
            if (!rebuilt.completed()) {
                if (rebuilt.failed()) {
                    // A refresh is transactional: keep serving the last complete
                    // index, but expose repeated filesystem/permission failures to
                    // metrics instead of silently turning them into stale content.
                    failures_.fetch_add(1, std::memory_order_relaxed);
                }
                if (!httpServerWorkerRunning(state_)) {
                    co_return;
                }
                continue;
            }

            candidate = std::move(rebuilt).value();
        } catch (...) {
            // The offload wrapper reports queue/pool shutdown as a status, but
            // creating its one-shot channel or transporting a result can still
            // fail with an allocation/runtime exception. Refresh is best effort:
            // retain the last complete snapshot and keep the listener alive.
            failures_.fetch_add(1, std::memory_order_relaxed);
            if (!httpServerWorkerRunning(state_)) {
                co_return;
            }
            continue;
        }

        if (!httpServerWorkerRunning(state_)) {
            co_return;
        }
        if (StaticRootAccess::fingerprint(*candidate) ==
                StaticRootAccess::fingerprint(*currentRoot) &&
            StaticRootAccess::sameSnapshot(*candidate, *currentRoot)) {
            continue;
        }

        const auto precompression = precompression_options(options_);
        if (precompression.enabled()) {
            try {
                auto prepared =
                    co_await ruvia::tryRunBlocking(*options_.blockingPool, worker_,
                        [candidate = std::move(candidate), precompression]() mutable {
                            StaticRootAccess::installPrecompressedVariants(
                                *candidate, nullptr, precompression);
                            return std::move(candidate);
                        });
                if (!prepared.completed()) {
                    if (prepared.failed()) {
                        failures_.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!httpServerWorkerRunning(state_)) {
                        co_return;
                    }
                    continue;
                }
                candidate = std::move(prepared).value();
            } catch (...) {
                failures_.fetch_add(1, std::memory_order_relaxed);
                if (!httpServerWorkerRunning(state_)) {
                    co_return;
                }
                continue;
            }
            if (!httpServerWorkerRunning(state_)) {
                co_return;
            }
        }

        // A binding is a request-scoped lease. If no request can still hold
        // the current index, replacing it may destroy the old root directly;
        // otherwise retain the old immutable snapshot until every in-flight
        // dispatch releases its move-only binding. Publishing a raw pointer
        // without this retirement step leaves a suspended coroutine with a
        // dangling StaticRoot after the next poll.
        try {
            if (current_ != nullptr &&
                StaticRootAccess::hasActiveBindings(*current_)) {
                // A vector growth is the only fallible part of publication.
                // The old pointer remains owned by this server if growth is
                // rejected, so the candidate can be discarded and the next
                // poll can retry without exposing a half-published root.
                retired_.push_back(std::move(current_));
            }
        } catch (...) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        current_ = std::move(candidate);
        options_.documentRoot.publish(*current_);
    }
}

}  // namespace ruvia::detail
