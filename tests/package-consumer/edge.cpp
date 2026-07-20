#include <ruvia/edge/EdgeConfig.h>

int main() {
    ruvia::edge::EdgeConfig config;
    ruvia::edge::OriginSettings settings;
    settings.upstreamHost = "origin.local";
    settings.upstreamPort = 8080;
    config.addOrigin("front.local", settings);
    const auto snapshot = config.snapshot();
    return snapshot->findOrigin("front.local") != nullptr ? 0 : 1;
}
