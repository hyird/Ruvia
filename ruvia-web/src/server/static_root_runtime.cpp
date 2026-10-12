#include "server/static_root_runtime.h"

#include <cstddef>
#include <exception>
#include <utility>

#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/memory/process_resource.h"
#include "ruvia/core/timer.h"

#include "server/http_server_options.h"

namespace ruvia::detail {
namespace {

using snapshot_owner = std::unique_ptr<static_root, pmr_object_deleter<static_root>>;

// Snapshots are released by whichever thread drops the last reference, so the
// control block and the root both come from the process resource.
[[nodiscard]] document_root_snapshot share_snapshot(snapshot_owner owner) {
    const auto deleter = owner.get_deleter();
    // The constructor invokes the deleter itself if its allocation fails.
    return document_root_snapshot(
        owner.release(), deleter, std::pmr::polymorphic_allocator<std::byte>(process_resource()));
}

}  // namespace

document_root_publisher::document_root_publisher(const std::filesystem::path& root,
    const static_root_config_storage& config, std::chrono::milliseconds interval,
    static_root_precompression_options precompression, static_root_resident_options resident)
    : interval_(interval),
      precompression_(precompression),
      resident_(resident) {
    auto initial = static_root_access::make(process_resource(), root, config);
    static_root_access::install_resident_bodies(*initial, nullptr, resident_);
    static_root_access::install_precompressed_variants(*initial, nullptr, precompression_);
    current_ = share_snapshot(std::move(initial));
}

document_root_publisher::~document_root_publisher() = default;

bool document_root_publisher::claim_refresh() noexcept {
    return !std::exchange(refresh_claimed_, true);
}

document_root_snapshot document_root_publisher::published_unless(const static_root* adopted) const {
    std::lock_guard lock(mutex_);
    if (current_.get() == adopted) {
        return nullptr;
    }
    return current_;
}

void document_root_publisher::publish(document_root_snapshot snapshot) {
    {
        std::lock_guard lock(mutex_);
        current_.swap(snapshot);
    }
    // The superseded snapshot is released outside the lock; workers that
    // still lease it keep their own reference.
}

task<void> document_root_publisher::refresh(
    blocking_pool& pool, const worker_handle& worker, const http_server_worker_state& state) {
    // Shared ownership lets the blocking jobs read the previous snapshot while
    // workers keep serving it, and lets resident bytes and precompression
    // reuse its unchanged file versions.
    const auto previous = current_;
    snapshot_owner candidate(nullptr, pmr_object_deleter<static_root>{process_resource()});
    try {
        auto rebuilt = co_await ruvia::try_run_blocking(pool, worker, [previous] {
            return static_root_access::make(process_resource(), previous->path(),
                static_root_access::copy_config(*previous, process_resource()));
        });
        if (!rebuilt.completed()) {
            // A refresh is transactional: keep serving the last complete
            // index, but expose filesystem/permission failures to metrics
            // instead of silently turning them into stale content. A rejected
            // offload (pool stopping) is not a refresh failure.
            if (rebuilt.failed()) {
                record_failure();
            }
            co_return;
        }
        candidate = std::move(rebuilt).value();
    } catch (...) {
        // The offload wrapper reports pool shutdown as a status, but creating
        // its one-shot channel or transporting a result can still throw.
        record_failure();
        co_return;
    }

    if (!http_server_worker_running(state)) {
        co_return;
    }
    if (static_root_access::fingerprint(*candidate) == static_root_access::fingerprint(*previous) &&
        static_root_access::same_snapshot(*candidate, *previous)) {
        co_return;
    }

    if (precompression_.enabled() || resident_.enabled()) {
        try {
            auto prepared = co_await ruvia::try_run_blocking(pool, worker,
                [candidate = std::move(candidate), previous, precompression = precompression_,
                    resident = resident_]() mutable {
                    static_root_access::install_resident_bodies(*candidate, previous.get(), resident);
                    static_root_access::install_precompressed_variants(
                        *candidate, previous.get(), precompression);
                    return std::move(candidate);
                });
            if (!prepared.completed()) {
                if (prepared.failed()) {
                    record_failure();
                }
                co_return;
            }
            candidate = std::move(prepared).value();
        } catch (...) {
            record_failure();
            co_return;
        }
        if (!http_server_worker_running(state)) {
            co_return;
        }
    }

    try {
        publish(share_snapshot(std::move(candidate)));
    } catch (...) {
        record_failure();
    }
}

static_root_runtime::static_root_runtime(http_server_options& options, const worker_handle& worker_value,
    const http_server_worker_state& state_value, std::pmr::memory_resource* resource)
    : options_(options),
      publisher_(options.document_root_.publisher()),
      worker_(worker_value),
      state_(state_value),
      resource_(resource),
      current_(nullptr, pmr_object_deleter<document_root_generation>{resource}),
      retired_(resource),
      refreshes_(publisher_ != nullptr && publisher_->claim_refresh()) {
    if (publisher_ != nullptr) {
        current_ = make_pmr_object<document_root_generation>(resource_, publisher_->published_unless(nullptr));
        options_.document_root_.publish(*current_);
    }
}

static_root_runtime::~static_root_runtime() = default;

void static_root_runtime::adopt_published() {
    auto published = publisher_->published_unless(current_->root());
    if (published == nullptr) {
        return;
    }
    auto next = make_pmr_object<document_root_generation>(resource_, std::move(published));
    if (current_->leased()) {
        // Vector growth is the only fallible step. If it is rejected the
        // worker keeps its current generation and retries next interval.
        retired_.push_back(std::move(current_));
    }
    current_ = std::move(next);
    options_.document_root_.publish(*current_);
}

task<void> static_root_runtime::refresh() {
    if (publisher_ == nullptr) {
        std::terminate();
    }
    const auto interval = publisher_->interval();
    for (;;) {
        if (!http_server_worker_running(state_)) {
            co_return;
        }
        if (co_await sleep_for(worker_, interval) == timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!http_server_worker_running(state_)) {
            co_return;
        }

        std::erase_if(retired_, [](const generation_owner& generation) { return !generation->leased(); });

        if (refreshes_) {
            co_await publisher_->refresh(*options_.blocking_pool_, worker_, state_);
            if (!http_server_worker_running(state_)) {
                co_return;
            }
        }
        try {
            adopt_published();
        } catch (...) {
            // This worker keeps serving its last generation; the snapshot it
            // missed is still published and is retried next interval.
            publisher_->record_failure();
        }
    }
}

}  // namespace ruvia::detail
