#include "document_root_publisher.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "blocking_pool.h"
#include "connection_lifecycle.h"
#include "failure_report.h"
#include "server_options.h"
#include "static_root.h"
#include "static_root_index.h"
#include "stop_token.h"
#include "task.h"
#include "worker_ref.h"
#include "worker_timer.h"

namespace ruvia::detail {

namespace {

using snapshot_owner = std::unique_ptr<static_root>;

// Snapshots are released by whichever thread drops the last reference, so the
// control block and the root both come from the process resource.
[[nodiscard]] document_root_snapshot share_snapshot(snapshot_owner owner) {
    return document_root_snapshot(std::move(owner));
}

}  // namespace

document_root_publisher::document_root_publisher(const std::filesystem::path& root, const static_root_config_storage& config, std::chrono::milliseconds interval, static_root_precompression_options precompression, static_root_resident_options resident, std::size_t worker_count) :
    root_(root),
    interval_(interval),
    precompression_(precompression),
    resident_(resident),
    mailboxes_(worker_count) {
    auto initial = static_root_access::make(root, config);
    static_root_access::install_resident_bodies(*initial, nullptr, resident_);
    static_root_access::install_precompressed_variants(*initial, nullptr, precompression_);
    current_ = share_snapshot(std::move(initial));
}

document_root_publisher::~document_root_publisher() = default;

document_root_publisher::mailbox::~mailbox() {
    // The graph has joined every worker before destroying the mailbox host.
    delete pending_.exchange(nullptr);
}

document_root_publisher::mailbox& document_root_publisher::register_worker() noexcept {
    if (registered_workers_ == mailboxes_.size()) {
        std::terminate();
    }
    return mailboxes_[registered_workers_++];
}

void document_root_publisher::record_failure(std::exception_ptr failure) noexcept {
    failures_.fetch_add(1, std::memory_order_relaxed);
    ruvia::report_unhandled_failure("document root refresh", std::move(failure));
}

void document_root_publisher::publish(document_root_snapshot snapshot) {
    std::vector<publication_owner> publication;
    publication.reserve(mailboxes_.size());
    for (std::size_t i = 0; i < mailboxes_.size(); ++i) {
        publication.push_back(std::make_unique<document_root_snapshot>(snapshot));
    }
    // Every allocation has succeeded before any worker can see this snapshot.
    commit_publication(std::move(snapshot), publication);
}

void document_root_publisher::commit_publication(document_root_snapshot snapshot, std::vector<publication_owner>& publication) noexcept {
    current_.swap(snapshot);
    for (std::size_t i = 0; i < mailboxes_.size(); ++i) {
        publication_owner superseded(mailboxes_[i].pending_.exchange(publication[i].release(), std::memory_order_acq_rel));
    }
    // Unread nodes are reclaimed by the next publish; adopted snapshots live
    // only as long as a worker's current generation or request leases need them.
}

task<void> document_root_publisher::refresh(const worker_ref& worker, const http_server_worker_state& state, const stop_token& stop) {
    // Shared ownership lets the blocking jobs read the previous snapshot while
    // workers keep serving it, and lets resident bytes and precompression
    // reuse its unchanged file versions.
    const auto previous = current_;
    snapshot_owner candidate(nullptr);
    try {
        // The unchanged-snapshot comparison walks every entry, so it runs on
        // the pool with the rebuild; an unchanged root yields no candidate.
        auto rebuilt = co_await ruvia::run_blocking(worker, {.stop_token_ = stop}, [previous, root = root_] {
            auto built = static_root_access::make(root, static_root_access::copy_config(*previous));
            if (static_root_access::fingerprint(*built) == static_root_access::fingerprint(*previous) && static_root_access::same_snapshot(*built, *previous)) {
                built.reset();
            }
            return built;
        });
        if (!rebuilt.completed()) {
            // A refresh is transactional: keep serving the last complete
            // index, but expose filesystem/permission failures to metrics
            // instead of silently turning them into stale content. A rejected
            // offload (pool stopping) is not a refresh failure.
            if (rebuilt.failed()) {
                record_failure(rebuilt.error());
            }
            co_return;
        }
        candidate = std::move(rebuilt).take_value();
    } catch (...) {
        // The offload wrapper reports pool shutdown as a status, but creating
        // its one-shot channel or transporting a result can still throw.
        record_failure(std::current_exception());
        co_return;
    }

    if (candidate == nullptr || !http_server_worker_running(state)) {
        co_return;
    }

    if (precompression_.enabled() || resident_.enabled()) {
        try {
            auto prepared = co_await ruvia::run_blocking(worker, {.stop_token_ = stop}, [candidate = std::move(candidate), previous, precompression = precompression_, resident = resident_]() mutable {
                static_root_access::install_resident_bodies(*candidate, previous.get(), resident);
                static_root_access::install_precompressed_variants(*candidate, previous.get(), precompression);
                return std::move(candidate);
            });
            if (!prepared.completed()) {
                if (prepared.failed()) {
                    record_failure(prepared.error());
                }
                co_return;
            }
            candidate = std::move(prepared).take_value();
        } catch (...) {
            record_failure(std::current_exception());
            co_return;
        }
        if (!http_server_worker_running(state)) {
            co_return;
        }
    }

    try {
        publish(share_snapshot(std::move(candidate)));
    } catch (...) {
        record_failure(std::current_exception());
    }
}

static_root_runtime::static_root_runtime(http_server_options& options, const worker_ref& worker_value, const http_server_worker_state& state_value) :
    options_(options),
    publisher_(options.document_root_.publisher()),
    mailbox_(publisher_ == nullptr ? nullptr : &publisher_->register_worker()),
    worker_(worker_value),
    state_(state_value),
    pending_(nullptr),
    current_(nullptr),
    retired_(),
    refreshes_(publisher_ != nullptr && mailbox_ == publisher_->mailboxes_.data()) {
    if (publisher_ != nullptr) {
        // Worker construction is serialized before startup; no publisher can
        // replace this shared_ptr while its initial reference is copied.
        current_ = std::make_unique<document_root_generation>(publisher_->current_);
        options_.document_root_.publish(*current_);
    }
}

static_root_runtime::~static_root_runtime() = default;

void static_root_runtime::adopt_published() {
    if (auto* published = mailbox_->pending_.exchange(nullptr, std::memory_order_acq_rel); published != nullptr) {
        pending_.reset(published);
    }
    if (pending_ == nullptr) {
        return;
    }
    // The exchanged node is exclusively ours, so copying its shared_ptr cannot
    // race the publisher reclaiming or replacing the source object.
    auto next = std::make_unique<document_root_generation>(*pending_);
    if (current_->leased()) {
        // If vector growth fails both current_ and pending_ remain owned here;
        // adoption retries next interval, or skips to a newer mailbox node.
        retired_.push_back(std::move(current_));
    }
    current_ = std::move(next);
    options_.document_root_.publish(*current_);
    pending_.reset();
}

task<void> static_root_runtime::refresh(const stop_token& stop) {
    if (publisher_ == nullptr) {
        std::terminate();
    }
    const auto interval = publisher_->interval();
    for (;;) {
        if (!http_server_worker_running(state_)) {
            co_return;
        }
        if (co_await sleep_for(worker_, interval, stop) == timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!http_server_worker_running(state_)) {
            co_return;
        }

        std::erase_if(retired_, [](const generation_owner& generation) {
            return !generation->leased();
        });

        if (refreshes_) {
            co_await publisher_->refresh(worker_, state_, stop);
            if (!http_server_worker_running(state_)) {
                co_return;
            }
        }
        try {
            adopt_published();
        } catch (...) {
            // This worker keeps serving its last generation and retains the
            // exclusively claimed publication node for the next interval.
            publisher_->record_failure(std::current_exception());
        }
    }
}

}  // namespace ruvia::detail
