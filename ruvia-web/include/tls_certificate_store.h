#pragma once

#include <cstddef>
#include <memory>

#include "task.h"
#include "tls_config.h"
#include "worker_ref.h"

namespace ruvia {

namespace detail {
struct tls_certificate_store_access;
}

// Thread-safe, independently usable certificate library. Writers serialize
// without blocking workers, prepare immutable snapshots on caller's blocking
// pool, then atomically publish on caller. Failed/rejected updates leave the
// library unchanged. Handshakes capture a snapshot before calling the selector:
// replacing/erasing its selected ID cannot invalidate that handshake.
// Established connections retain their OpenSSL references. Cold tasks retain
// the library state and own their inputs even after this object is destroyed.
// Copies are deliberately disabled; tls_config retains shared ownership.
class tls_certificate_store final {
public:
    tls_certificate_store();
    ~tls_certificate_store();
    tls_certificate_store(const tls_certificate_store&) = delete;
    tls_certificate_store& operator=(const tls_certificate_store&) = delete;

    // Await on caller; awaiting elsewhere throws std::logic_error. The caller
    // must have an attached blocking_pool for parsing or snapshot preparation.
    // Rejections throw blocking_operation_rejected; loader exceptions propagate
    // unchanged. A stopping caller cannot publish: std::runtime_error before
    // submission/after preparation, or blocking_operation_rejected while
    // waiting for the pool. Started tasks must be awaited/joined.
    // ID zero throws std::invalid_argument. Before returning a cold task, the
    // library copies secrets into heap storage and cleanses its
    // incoming copy, including on failure. Discarding a cold task cleanses its
    // retained copy; any detached preparation owns and cleanses its own input
    // and never publishes. These operations have no per-call timeout/token.
    [[nodiscard]] task<void> insert_or_assign(worker_ref caller, tls_certificate_id id, tls_identity_config identity);
    // Returns false for an absent ID without submitting blocking work.
    [[nodiscard]] task<bool> erase(worker_ref caller, tls_certificate_id id);
    [[nodiscard]] bool contains(tls_certificate_id id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    friend struct detail::tls_certificate_store_access;
    struct state;
    std::shared_ptr<state> state_;
};

}  // namespace ruvia
