#pragma once

#include <cstddef>
#include <exception>
#include <utility>

#include "ruvia/web/detail/http/static/StaticRootIndex.h"

namespace ruvia {

class StaticRoot;

namespace detail {

// A request-time view of one document-root binding. The object is deliberately
// not default-constructible or aggregate-initializable: callers must choose
// between no root, a standalone immutable root, and a server-configured root.
// It is move-only because a configured binding is also
// the request's lifetime lease for its worker-owned immutable root snapshot.
// A server retires the old snapshot until this lease is destroyed; copying the
// view would make that ownership boundary ambiguous and could reintroduce a
// dangling StaticRoot during live refresh. An application-owned immutable root
// outlives every worker and therefore does not acquire a shared request lease.
class DocumentRootBinding final {
public:
    [[nodiscard]] static DocumentRootBinding none() noexcept {
        return DocumentRootBinding(nullptr, nullptr);
    }

    [[nodiscard]] static DocumentRootBinding standalone(const StaticRoot& root) noexcept {
        return DocumentRootBinding(&root, nullptr);
    }

    [[nodiscard]] static DocumentRootBinding configured(const StaticRoot& root) noexcept {
        return DocumentRootBinding(&root, &root);
    }

    ~DocumentRootBinding() {
        reset();
    }

    DocumentRootBinding(const DocumentRootBinding&) = delete;
    DocumentRootBinding& operator=(const DocumentRootBinding&) = delete;

    DocumentRootBinding(DocumentRootBinding&& other) noexcept
        : root_(std::exchange(other.root_, nullptr)),
          leasedRoot_(std::exchange(other.leasedRoot_, nullptr)) {}

    DocumentRootBinding& operator=(DocumentRootBinding&& other) noexcept {
        if (this != &other) {
            reset();
            root_ = std::exchange(other.root_, nullptr);
            leasedRoot_ = std::exchange(other.leasedRoot_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] const StaticRoot* root() const noexcept {
        return root_;
    }

private:
    [[nodiscard]] bool tracksSnapshot() const noexcept {
        return leasedRoot_ != nullptr;
    }

    explicit DocumentRootBinding(const StaticRoot* root, const StaticRoot* leasedRoot) noexcept
        : root_(root),
          leasedRoot_(leasedRoot) {
        if (tracksSnapshot()) {
            StaticRootAccess::acquireBinding(*leasedRoot_);
        }
    }

    void reset() noexcept {
        const auto* const leasedRoot = leasedRoot_;
        root_ = nullptr;
        leasedRoot_ = nullptr;
        if (leasedRoot != nullptr) {
            StaticRootAccess::releaseBinding(*leasedRoot);
        }
    }

    const StaticRoot* root_;
    const StaticRoot* leasedRoot_;
};

static_assert(sizeof(DocumentRootBinding) == 2 * sizeof(void*));

}  // namespace detail
}  // namespace ruvia
