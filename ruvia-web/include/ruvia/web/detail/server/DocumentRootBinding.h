#pragma once

#include <cstddef>
#include <exception>
#include <utility>

#include "ruvia/web/detail/http/static/StaticRootIndex.h"

namespace ruvia {

class StaticRoot;
struct DocumentRootRuntimeOptions;

namespace detail {

// A request-time view of one document-root binding. The object is deliberately
// not default-constructible or aggregate-initializable: callers must choose
// between no root, a standalone immutable root, and a server-configured root
// with its runtime policy. It is move-only because a configured binding is
// also the request's lifetime lease for the immutable root snapshot. A server
// retires the old snapshot until this lease is destroyed; copying the view
// would make that ownership boundary ambiguous and could reintroduce a
// dangling StaticRoot during live refresh.
class DocumentRootBinding final {
public:
    [[nodiscard]] static DocumentRootBinding none() noexcept {
        return DocumentRootBinding(nullptr, nullptr);
    }

    [[nodiscard]] static DocumentRootBinding standalone(const StaticRoot& root) noexcept {
        return DocumentRootBinding(&root, nullptr);
    }

    [[nodiscard]] static DocumentRootBinding configured(const StaticRoot& root, const DocumentRootRuntimeOptions& runtimeOptions) noexcept {
        return DocumentRootBinding(&root, &runtimeOptions);
    }

    ~DocumentRootBinding() {
        reset();
    }

    DocumentRootBinding(const DocumentRootBinding&) = delete;
    DocumentRootBinding& operator=(const DocumentRootBinding&) = delete;

    DocumentRootBinding(DocumentRootBinding&& other) noexcept
        : root_(std::exchange(other.root_, nullptr)),
          runtimeOptions_(std::exchange(other.runtimeOptions_, nullptr)) {}

    DocumentRootBinding& operator=(DocumentRootBinding&& other) noexcept {
        if (this != &other) {
            reset();
            root_ = std::exchange(other.root_, nullptr);
            runtimeOptions_ = std::exchange(other.runtimeOptions_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] const StaticRoot* root() const noexcept {
        return root_;
    }

    [[nodiscard]] const DocumentRootRuntimeOptions* runtimeOptions() const noexcept {
        return runtimeOptions_;
    }

private:
    explicit DocumentRootBinding(const StaticRoot* root, const DocumentRootRuntimeOptions* runtimeOptions) noexcept
        : root_(root),
          runtimeOptions_(runtimeOptions) {
        if (root_ != nullptr && runtimeOptions_ != nullptr) {
            StaticRootAccess::acquireBinding(*root_);
        }
    }

    void reset() noexcept {
        const auto* const root = root_;
        const bool tracked = root != nullptr && runtimeOptions_ != nullptr;
        root_ = nullptr;
        runtimeOptions_ = nullptr;
        if (tracked) {
            StaticRootAccess::releaseBinding(*root);
        }
    }

    const StaticRoot* root_;
    const DocumentRootRuntimeOptions* runtimeOptions_;
};

static_assert(sizeof(DocumentRootBinding) == 2 * sizeof(void*));

}  // namespace detail
}  // namespace ruvia
