#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/coding/HttpContentCoding.h"

namespace ruvia::detail {

enum class HttpContentEncodeStep : std::uint8_t {
    kProducedOrPending,
    kFinished,
    // Terminal: the encoder may no longer be written or finished after a
    // failure because a codec can have consumed input before output storage
    // reports an allocation failure.
    kFailure,
};

// Incremental response content encoder. The caller owns the output buffer and
// may clear/reuse it for every transport write. A stream owns one encoder from
// its response head until end(); buffered responses continue to use
// encodeHttpContent() so they retain their transactional whole-body behavior.
class HttpContentEncoder final {
public:
    struct Impl;

    HttpContentEncoder(HttpContentCoding coding, std::pmr::memory_resource* resource);
    ~HttpContentEncoder();

    HttpContentEncoder(const HttpContentEncoder&) = delete;
    HttpContentEncoder& operator=(const HttpContentEncoder&) = delete;
    HttpContentEncoder(HttpContentEncoder&&) = delete;
    HttpContentEncoder& operator=(HttpContentEncoder&&) = delete;

    [[nodiscard]] HttpContentCoding coding() const noexcept {
        return coding_;
    }

    // Encode one input chunk. When flush is true, the coding emits all bytes
    // currently visible to the peer; this is needed for low-latency SSE.
    [[nodiscard]] HttpContentEncodeStep write(std::string_view input, std::pmr::string& output, bool flush = false);

    // Finish the representation and append the final coding bytes.
    [[nodiscard]] HttpContentEncodeStep finish(std::pmr::string& output);

private:
    HttpContentCoding coding_;
    std::pmr::memory_resource* resource_;
    Impl* impl_;
    bool finished_{false};
    bool failed_{false};
};

}  // namespace ruvia::detail
