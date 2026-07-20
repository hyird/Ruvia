// EdgeConfig is the edge node's dynamically mutable routing table: reads are the
// lock-free hot path, writes (add/remove origin) publish a fresh immutable
// snapshot via copy-on-write. This checks the mutation results and, crucially,
// that a snapshot captured before a mutation is never disturbed by it.

#include <cstdio>

#include "ruvia/edge/EdgeConfig.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

}  // namespace

int main() {
    using ruvia::edge::EdgeConfig;
    using ruvia::edge::OriginSettings;

    EdgeConfig config;

    {
        const auto snap = config.snapshot();
        check(snap->originCount() == 0, "initial config is empty");
        check(snap->findOrigin("example.com") == nullptr,
              "unmapped host resolves to nullptr");
    }

    check(config.addOrigin("example.com", OriginSettings{"backend.internal", 8080, false}),
          "addOrigin returns true for a new mapping");
    {
        const auto snap = config.snapshot();
        check(snap->originCount() == 1, "count is 1 after add");
        const auto* origin = snap->findOrigin("example.com");
        check(origin != nullptr, "mapped host resolves");
        check(origin != nullptr && origin->upstreamHost == "backend.internal",
              "upstream host stored");
        check(origin != nullptr && origin->upstreamPort == 8080,
              "upstream port stored");
        check(origin != nullptr && !origin->https, "https flag stored");
    }

    // Copy-on-write: a snapshot taken before a later mutation stays frozen.
    {
        const auto oldSnap = config.snapshot();
        config.addOrigin("second.com", OriginSettings{"other.internal", 443, true});
        check(oldSnap->originCount() == 1,
              "old snapshot unchanged after later add (copy-on-write)");
        check(oldSnap->findOrigin("second.com") == nullptr,
              "old snapshot does not observe the new host");
        const auto newSnap = config.snapshot();
        check(newSnap->originCount() == 2, "new snapshot sees both hosts");
        const auto* origin = newSnap->findOrigin("second.com");
        check(origin != nullptr && origin->https, "https origin stored");
    }

    // Replacing an existing mapping returns false and updates the settings.
    check(!config.addOrigin("example.com", OriginSettings{"newbackend", 9090, true}),
          "addOrigin returns false when replacing an existing host");
    {
        const auto snap = config.snapshot();
        check(snap->originCount() == 2, "count unchanged after replace");
        const auto* origin = snap->findOrigin("example.com");
        check(origin != nullptr && origin->upstreamHost == "newbackend" &&
                  origin->upstreamPort == 9090,
              "replaced origin reflects new settings");
    }

    check(config.removeOrigin("example.com"),
          "removeOrigin returns true when the host is present");
    check(!config.removeOrigin("example.com"),
          "removeOrigin returns false when the host is absent");
    {
        const auto snap = config.snapshot();
        check(snap->originCount() == 1, "count is 1 after remove");
        check(snap->findOrigin("example.com") == nullptr,
              "removed host resolves to nullptr");
        check(snap->findOrigin("second.com") != nullptr,
              "unrelated host is still present");
    }

    if (failures == 0) {
        std::fprintf(stderr, "edge config: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
