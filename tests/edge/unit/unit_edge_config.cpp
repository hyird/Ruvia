// EdgeConfig is single-owner state. Lookups return a move-only lease whose
// lifetime remains stable across later replacement/removal without atomic
// shared ownership on the request path.

#include <cstdio>

#include "ruvia/edge/detail/EdgeConfig.h"

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
    check(config.originCount() == 0, "initial config is empty");
    check(!config.findOrigin("example.com"), "unmapped host has no lease");

    check(config.addOrigin("example.com", OriginSettings{"backend.internal", 8080, false}),
          "addOrigin returns true for a new mapping");
    check(config.originCount() == 1, "count is 1 after add");

    auto original = config.findOrigin("example.com");
    check(static_cast<bool>(original), "mapped host resolves");
    check(original && original->upstreamHost == "backend.internal",
          "upstream host stored");
    check(original && original->upstreamPort == 8080, "upstream port stored");
    check(original && !original->https, "https flag stored");
    check(static_cast<bool>(config.findOrigin("EXAMPLE.COM")),
          "front host lookup is ASCII case-insensitive");

    check(!config.addOrigin("Example.Com", OriginSettings{"newbackend", 9090, true}),
          "addOrigin returns false when replacing an existing host");
    check(config.originCount() == 1, "count unchanged after replace");
    auto replacement = config.findOrigin("example.com");
    check(replacement && replacement->upstreamHost == "newbackend" &&
              replacement->upstreamPort == 9090 && replacement->https,
          "new lookup sees replacement settings");
    check(original && original->upstreamHost == "backend.internal" &&
              original->upstreamPort == 8080 && !original->https,
          "outstanding lease survives replacement with its original value");

    check(config.removeOrigin("EXAMPLE.com"),
          "removeOrigin returns true when the host is present");
    check(!config.removeOrigin("example.com"),
          "removeOrigin returns false when the host is absent");
    check(config.originCount() == 0, "count is 0 after remove");
    check(!config.findOrigin("example.com"), "removed host no longer resolves");
    check(replacement && replacement->upstreamHost == "newbackend",
          "outstanding lease survives removal");

    if (failures == 0) {
        std::fprintf(stderr, "edge config: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
