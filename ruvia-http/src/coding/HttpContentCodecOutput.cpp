#include "ruvia/http/detail/coding/HttpContentCodec.h"

#include "ruvia/http/detail/util/PmrResource.h"

// The output ceiling every decoder shares.

namespace ruvia::detail {

bool appendDecodedBytes(
    std::pmr::string& output,
    const char* bytes,
    std::size_t size,
    std::size_t maxDecodedBytes) {
    if (output.size() > maxDecodedBytes ||
        size > maxDecodedBytes - output.size()) {
        return false;
    }
    output.append(bytes, size);
    return true;
}

}  // namespace ruvia::detail
