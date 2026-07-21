#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ruvia::edge {

// Where a front-facing host is proxied to. Owned storage: the configuration
// outlives every request that reads it, unlike the borrowed HttpOrigin used at
// fetch time. IP-literal hosts keep their brackets (for example "[::1]").
struct OriginSettings final {
    std::string upstreamHost;
    std::uint16_t upstreamPort{80};
    bool https{false};
};

// An immutable view of the edge routing table. EdgeConfig publishes a fresh
// snapshot on every mutation, so a request handler reads a stable, fully-formed
// configuration without locking and without racing a concurrent control-plane
// change.
class EdgeConfigSnapshot final {
public:
    // Resolve the origin for a front-facing Host value. The lookup is
    // heterogeneous, so passing a borrowed request Host does not allocate.
    // Returns nullptr when the host is not mapped.
    [[nodiscard]] const OriginSettings* findOrigin(
        std::string_view frontHost) const noexcept;

    [[nodiscard]] std::size_t originCount() const noexcept {
        return origins_.size();
    }

private:
    friend class EdgeConfig;

    struct TransparentHash final {
        using is_transparent = void;
        [[nodiscard]] std::size_t operator()(
            std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    std::unordered_map<
        std::string,
        OriginSettings,
        TransparentHash,
        std::equal_to<>>
        origins_;
};

// The live, dynamically mutable edge configuration.
//
// Reads (snapshot()) are the hot request path and take no lock: they atomically
// load the currently published immutable snapshot. Writes (addOrigin/
// removeOrigin) are the rare control-plane path: each copies the current
// snapshot, applies the change, and atomically publishes the new snapshot, so a
// reader either sees the whole change or none of it -- never a half-applied
// table. Writes are serialized by an internal mutex to prevent lost updates.
class EdgeConfig final {
public:
    EdgeConfig();

    EdgeConfig(const EdgeConfig&) = delete;
    EdgeConfig& operator=(const EdgeConfig&) = delete;

    // Hot path. Hold the returned pointer for the whole request: it keeps that
    // snapshot alive even if a mutation republishes a newer one meanwhile.
    [[nodiscard]] std::shared_ptr<const EdgeConfigSnapshot>
    snapshot() const noexcept;

    // Control plane. Add or replace the origin for a front-facing host. Returns
    // true when a new mapping was created, false when an existing one was
    // replaced.
    bool addOrigin(std::string frontHost, OriginSettings settings);

    // Control plane. Remove the mapping for a front-facing host. Returns true
    // when a mapping was removed, false when the host was not mapped (in which
    // case no new snapshot is published).
    bool removeOrigin(std::string_view frontHost);

private:
    using SnapshotPtr = std::shared_ptr<const EdgeConfigSnapshot>;

    [[nodiscard]] SnapshotPtr loadSnapshot(
        std::memory_order order) const noexcept;
    void storeSnapshot(SnapshotPtr snapshot, std::memory_order order) noexcept;

    std::mutex writeMutex_;
#if defined(__cpp_lib_atomic_shared_ptr)
    std::atomic<SnapshotPtr> current_;
#else
    SnapshotPtr current_;
#endif
};

}  // namespace ruvia::edge
