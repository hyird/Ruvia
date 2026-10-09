#pragma once

#include <cstddef>
#include <exception>
#include <utility>

#include "http/static_root_index.h"

namespace ruvia {

class static_root;

namespace detail {

// A request-time view of one document-root binding. The object is deliberately
// not default-constructible or aggregate-initializable: callers must choose
// between no root, a standalone immutable root, and a server-configured root.
// It is move-only because a configured binding is also
// the request's lifetime lease for its worker-owned immutable root snapshot.
// A server retires the old snapshot until this lease is destroyed; copying the
// view would make that ownership boundary ambiguous and could reintroduce a
// dangling static_root during live refresh. An application-owned immutable root
// outlives every worker and therefore does not acquire a shared request lease.
class document_root_binding final {
public:
    [[nodiscard]] static document_root_binding none() noexcept {
        return document_root_binding(nullptr, nullptr);
    }

    [[nodiscard]] static document_root_binding standalone(const static_root& root) noexcept {
        return document_root_binding(&root, nullptr);
    }

    [[nodiscard]] static document_root_binding configured(const static_root& root) noexcept {
        return document_root_binding(&root, &root);
    }

    ~document_root_binding() {
        reset();
    }

    document_root_binding(const document_root_binding&) = delete;
    document_root_binding& operator=(const document_root_binding&) = delete;

    document_root_binding(document_root_binding&& other) noexcept
        : root_(std::exchange(other.root_, nullptr)),
          leased_root_(std::exchange(other.leased_root_, nullptr)) {}

    document_root_binding& operator=(document_root_binding&& other) noexcept {
        if (this != &other) {
            reset();
            root_ = std::exchange(other.root_, nullptr);
            leased_root_ = std::exchange(other.leased_root_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] const static_root* root() const noexcept {
        return root_;
    }

private:
    [[nodiscard]] bool tracks_snapshot() const noexcept {
        return leased_root_ != nullptr;
    }

    explicit document_root_binding(const static_root* root, const static_root* leased_root) noexcept
        : root_(root),
          leased_root_(leased_root) {
        if (tracks_snapshot()) {
            static_root_access::acquire_binding(*leased_root_);
        }
    }

    void reset() noexcept {
        const auto* const leased_root = leased_root_;
        root_ = nullptr;
        leased_root_ = nullptr;
        if (leased_root != nullptr) {
            static_root_access::release_binding(*leased_root);
        }
    }

    const static_root* root_;
    const static_root* leased_root_;
};

static_assert(sizeof(document_root_binding) == 2 * sizeof(void*));

}  // namespace detail
}  // namespace ruvia
