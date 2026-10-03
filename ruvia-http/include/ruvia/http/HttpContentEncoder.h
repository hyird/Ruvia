#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/HttpContentCoding.h"

namespace ruvia {

class http_content_encoder_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Synchronous incremental representation encoder. Each write consumes all of
// its input and appends to caller-owned output; it does not retain either view.
// The PMR resource must outlive the stable-address encoder. Output can be cleared
// between calls and remains valid after encoder destruction.
// Codec failures throw http_content_encoder_error; allocation exceptions retain
// their original type when the codec returns through its C API. Brotli builds
// that exit on internal OOM cannot be recovered by this boundary.
// Either failure is terminal because input/output may have
// progressed. Later operations throw std::logic_error. finish() is idempotent;
// writing after successful finish throws std::logic_error without changing output.
class http_content_encoder final {
public:
    http_content_encoder(HttpContentCoding coding, std::pmr::memory_resource* resource);
    ~http_content_encoder();
    http_content_encoder(const http_content_encoder&) = delete;
    http_content_encoder& operator=(const http_content_encoder&) = delete;
    http_content_encoder(http_content_encoder&&) = delete;
    http_content_encoder& operator=(http_content_encoder&&) = delete;

    [[nodiscard]] HttpContentCoding coding() const noexcept {
        return coding_;
    }

    // flush emits pending bytes for low-latency response streams such as SSE.
    void write(std::string_view input, std::pmr::string& output, bool flush = false);
    void finish(std::pmr::string& output);

private:
    struct impl;
    enum class phase : unsigned char { active,
        finished,
        failed };
    HttpContentCoding coding_;
    std::pmr::memory_resource* resource_;
    impl* impl_{nullptr};
    phase phase_{phase::active};
};

}  // namespace ruvia
