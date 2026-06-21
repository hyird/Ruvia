#pragma once

#include "ruvia/http/detail/PmrString.h"
#include "ruvia/http/HttpParser.h"

#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include <zlib.h>

namespace ruvia::detail {

inline constexpr std::size_t kBodyReadChunkBytes = 8 * 1024;

[[noreturn]] void throwRequestBodyTooLarge();

class TransferCodingDecoder final {
public:
    TransferCodingDecoder(
        HttpTransferCodings codings,
        std::pmr::polymorphic_allocator<char> allocator,
        std::size_t maxBodyBytes);
    ~TransferCodingDecoder();

    TransferCodingDecoder(const TransferCodingDecoder&) = delete;
    TransferCodingDecoder& operator=(const TransferCodingDecoder&) = delete;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool finished() const noexcept;

    void setInput(std::string_view input);
    [[nodiscard]] std::string_view produce();
    void decodeAppend(std::string_view input, std::pmr::string& target);
    void finish();

private:
    struct InflateStep {
        std::size_t produced{0};
        int status{Z_OK};
    };

    [[nodiscard]] InflateStep inflateStep(char* out, std::size_t capacity) noexcept;
    void applyStatus(const InflateStep& step);

    struct alignas(std::max_align_t) ZlibAllocationHeader {
        std::pmr::memory_resource* resource;
        std::size_t bytes;
    };

    static voidpf zallocThunk(voidpf opaque, uInt items, uInt size) noexcept;
    static void zfreeThunk(voidpf opaque, voidpf address) noexcept;

    void cleanup() noexcept;
    void checkProducedLimit(std::size_t produced) const;

    HttpTransferCodings codings_;
    z_stream stream_{};
    bool initialized_{false};
    bool ended_{false};
    std::pmr::string output_;
    std::string_view pendingInput_;
    std::size_t pendingOffset_{0};
    std::pmr::memory_resource* resource_{nullptr};
    std::size_t maxBodyBytes_{0};
    std::size_t decodedBytes_{0};
};

}  // namespace ruvia::detail
