#include "ruvia/http/detail/http2/Http2Hpack.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace {

struct HeaderCounter final {
    std::size_t count{0};
};

bool countHeader(void* target, std::string_view, std::string_view) {
    auto& counter = *static_cast<HeaderCounter*>(target);
    ++counter.count;
    // Callback rejection is part of the decoder contract and must still leave the
    // dynamic table synchronized for a following field block.
    return counter.count <= 128;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }

    std::pmr::monotonic_buffer_resource resource;
    ruvia::detail::HpackDecoder decoder(&resource);
    decoder.setMaxDynamicTableSize(
        static_cast<std::size_t>(data[0]) * 32U);

    const auto payload = std::string_view(
        reinterpret_cast<const char*>(data + 1),
        size - 1);
    const auto split = payload.empty()
        ? std::size_t{0}
        : static_cast<std::size_t>(data[0]) % (payload.size() + 1U);

    HeaderCounter counter;
    (void)decoder.decode(payload.substr(0, split), &counter, &countHeader);
    (void)decoder.decode(payload.substr(split), &counter, &countHeader);
    return 0;
}
