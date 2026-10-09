#include "server/static_root_runtime.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/timer.h"

#include "http/static_root_index.h"
#include "server/http_server_options.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] static_root_precompression_options precompression_options(
    const http_server_options& options) noexcept {
    const auto* configured = options.document_root_.precompression_options();
    if (configured == nullptr || !options.compression_.has_value()) {
        return {};
    }
    return *configured;
}

}  // namespace

static_root_runtime::static_root_runtime(http_server_options& options, const worker_handle& worker_value,
    const http_server_worker_state& state_value, std::pmr::memory_resource* resource)
    : options_(options),
      worker_(worker_value),
      state_(state_value),
      current_(nullptr, pmr_object_deleter<static_root>{process_resource()}),
      retired_(resource) {
    if (options_.document_root_.refresh_options() != nullptr) {
        const auto* configured_root = options_.document_root_.root();
        if (configured_root == nullptr) {
            std::terminate();
        }
        current_ = static_root_access::clone(process_resource(), *configured_root);
        const auto precompression = precompression_options(options_);
        if (precompression.enabled()) {
            static_root_access::install_precompressed_variants(
                *current_, configured_root, precompression);
        }
        options_.document_root_.publish(*current_);
    }
}

static_root_runtime::~static_root_runtime() = default;

task<void> static_root_runtime::refresh() {
    const auto* refresh_options = options_.document_root_.refresh_options();
    if (refresh_options == nullptr) {
        std::terminate();
    }
    const auto interval = refresh_options->refresh_interval_;
    const auto reclaim_retired_roots = [this]() noexcept {
        std::erase_if(retired_, [](const snapshot_owner& root) {
            return root == nullptr || !static_root_access::has_active_bindings(*root);
        });
    };
    for (;;) {
        if (!http_server_worker_running(state_)) {
            co_return;
        }
        if (co_await sleep_for(worker_, interval) ==
            timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!http_server_worker_running(state_)) {
            co_return;
        }

        // Lease counts belong to the snapshot they protect. Reclaim old
        // generations independently; a long request on an older generation
        // must not pin every newer generation published by polling.
        reclaim_retired_roots();

        const auto* current_root = options_.document_root_.root();
        if (current_root == nullptr) {
            // The validated document-root configuration owns this invariant. Keep
            // the loop defensive anyway: a broken runtime binding must not
            // turn a background task into a null dereference on the worker.
            failures_.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }

        std::filesystem::path root_path;
        std::optional<static_root_config_storage> root_config;
        try {
            // Both operations copy PMR-backed configuration. They are outside
            // try_run_blocking because the source snapshot is worker-owned, but a
            // transient allocation failure here is still a refresh failure,
            // not a reason to terminate the listener and discard its last
            // complete index.
            root_path = current_root->path();
            root_config.emplace(static_root_access::copy_config(*current_root, process_resource()));
        } catch (...) {
            failures_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        snapshot_owner candidate_value(nullptr, pmr_object_deleter<static_root>{process_resource()});
        try {
            auto rebuilt = co_await ruvia::try_run_blocking(*options_.blocking_pool_,
                worker_,
                [root_path = std::move(root_path), root_config = std::move(*root_config)]() mutable {
                    return static_root_access::make(
                        process_resource(), root_path, std::move(root_config));
                });
            if (!rebuilt.completed()) {
                if (rebuilt.failed()) {
                    // A refresh is transactional: keep serving the last complete
                    // index, but expose repeated filesystem/permission failures to
                    // metrics instead of silently turning them into stale content.
                    failures_.fetch_add(1, std::memory_order_relaxed);
                }
                if (!http_server_worker_running(state_)) {
                    co_return;
                }
                continue;
            }

            candidate_value = std::move(rebuilt).value();
        } catch (...) {
            // The offload wrapper reports queue/pool shutdown as a status, but
            // creating its one-shot channel or transporting a result can still
            // fail with an allocation/runtime exception. Refresh is best effort:
            // retain the last complete snapshot and keep the listener alive.
            failures_.fetch_add(1, std::memory_order_relaxed);
            if (!http_server_worker_running(state_)) {
                co_return;
            }
            continue;
        }

        if (!http_server_worker_running(state_)) {
            co_return;
        }
        if (static_root_access::fingerprint(*candidate_value) ==
                static_root_access::fingerprint(*current_root) &&
            static_root_access::same_snapshot(*candidate_value, *current_root)) {
            continue;
        }

        const auto precompression = precompression_options(options_);
        if (precompression.enabled()) {
            try {
                auto prepared =
                    co_await ruvia::try_run_blocking(*options_.blocking_pool_, worker_,
                        [candidate_value = std::move(candidate_value), precompression]() mutable {
                            static_root_access::install_precompressed_variants(
                                *candidate_value, nullptr, precompression);
                            return std::move(candidate_value);
                        });
                if (!prepared.completed()) {
                    if (prepared.failed()) {
                        failures_.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!http_server_worker_running(state_)) {
                        co_return;
                    }
                    continue;
                }
                candidate_value = std::move(prepared).value();
            } catch (...) {
                failures_.fetch_add(1, std::memory_order_relaxed);
                if (!http_server_worker_running(state_)) {
                    co_return;
                }
                continue;
            }
            if (!http_server_worker_running(state_)) {
                co_return;
            }
        }

        // A binding is a request-scoped lease. If no request can still hold
        // the current index, replacing it may destroy the old root directly;
        // otherwise retain the old immutable snapshot until every in-flight
        // dispatch releases its move-only binding. Publishing a raw pointer
        // without this retirement step leaves a suspended coroutine with a
        // dangling static_root after the next poll.
        try {
            if (current_ != nullptr &&
                static_root_access::has_active_bindings(*current_)) {
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
        current_ = std::move(candidate_value);
        options_.document_root_.publish(*current_);
    }
}

}  // namespace ruvia::detail
