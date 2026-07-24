#pragma once

#include <cstddef>
#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ruvia/edge/EdgeTypes.h"
#include "ruvia/http/detail/util/AsciiCase.h"

namespace ruvia::edge {

struct EdgeOriginControl;

// A stable reference to one resolved origin. The lease is move-only because a
// request needs exactly one owner while it may suspend. Replacing or removing
// the mapping does not invalidate an already-resolved lease.
//
// EdgeConfig and every outstanding lease are confined to the same owner thread.
// The reference count is deliberately non-atomic: EdgeServer marshals control
// operations onto its event-loop thread instead of charging every request for
// cross-thread ownership.
class OriginLease final {
public:
    OriginLease() noexcept = default;
    ~OriginLease();

    OriginLease(const OriginLease&) = delete;
    OriginLease& operator=(const OriginLease&) = delete;
    OriginLease(OriginLease&& other) noexcept;
    OriginLease& operator=(OriginLease&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return control_ != nullptr;
    }
    [[nodiscard]] const OriginSettings* get() const noexcept;
    [[nodiscard]] const OriginSettings& operator*() const noexcept;
    [[nodiscard]] const OriginSettings* operator->() const noexcept;

private:
    friend class EdgeConfig;
    explicit OriginLease(EdgeOriginControl* control) noexcept;

    EdgeOriginControl* control_{nullptr};
};

// Internal mutable origin table owned by one event-loop thread. Neither lookup nor
// mutation performs synchronization. The embedding runtime must marshal every
// operation onto that owner; EdgeServer does so for its public control plane.
class EdgeConfig final {
public:
    explicit EdgeConfig(std::pmr::memory_resource* resource = std::pmr::get_default_resource()) noexcept;
    ~EdgeConfig();

    EdgeConfig(const EdgeConfig&) = delete;
    EdgeConfig& operator=(const EdgeConfig&) = delete;

    // Heterogeneous lookup: a borrowed request Host probes without allocating.
    // The returned lease remains valid across suspension and later mutations.
    [[nodiscard]] OriginLease findOrigin(std::string_view frontHost) noexcept;

    [[nodiscard]] std::size_t originCount() const noexcept {
        return origins_.size();
    }

    // Add or replace a mapping. Returns true for insertion and false for
    // replacement. Existing leases continue to refer to the old settings.
    bool addOrigin(std::string frontHost, OriginSettings settings);

    // Remove a mapping. Existing leases remain valid until released.
    bool removeOrigin(std::string_view frontHost) noexcept;

private:
    struct TransparentHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
            // Host/reg-name matching is ASCII case-insensitive. Keep the hash
            // transparent so a request view still probes without allocation.
            std::size_t hash = static_cast<std::size_t>(1469598103934665603ULL);
            for (const unsigned char c : value) {
                hash ^= ::ruvia::detail::httpAsciiToLower(c);
                hash *= static_cast<std::size_t>(1099511628211ULL);
            }
            return hash;
        }
    };
    struct TransparentEqual final {
        using is_transparent = void;
        template <typename Left, typename Right>
        [[nodiscard]] bool operator()(const Left& left, const Right& right) const noexcept {
            return ::ruvia::detail::httpAsciiEqualsIgnoreCase(std::string_view(left), std::string_view(right));
        }
    };

    std::pmr::memory_resource* resource_;
    std::pmr::unordered_map<std::pmr::string, EdgeOriginControl*, TransparentHash, TransparentEqual> origins_;
};

}  // namespace ruvia::edge
