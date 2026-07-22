#include "ruvia/edge/detail/cache/CacheKey.h"

#include "ruvia/edge/detail/proxy/HeaderRules.h"

namespace ruvia::edge {

std::string cacheVariantPrefix(
    std::string_view method,
    std::string_view frontHost,
    std::string_view target) {
    std::string key;
    key.reserve(method.size() + frontHost.size() + target.size() + 3);
    key.append(method);
    key.push_back('\n');
    for (const char c : frontHost) {
        key.push_back(toLowerAscii(c));
    }
    key.push_back('\n');
    key.append(target);
    key.push_back('\n');
    return key;
}

std::string cacheKeyFor(
    std::string_view variantPrefix,
    std::string_view host,
    const std::optional<std::string>& acceptEncoding) {
    std::string key(variantPrefix);
    for (const char byte : host) {
        key.push_back(toLowerAscii(byte));
    }
    key.push_back('\n');
    key.push_back(acceptEncoding ? '1' : '0');
    if (acceptEncoding) {
        key.append(*acceptEncoding);
    }
    return key;
}

}  // namespace ruvia::edge
