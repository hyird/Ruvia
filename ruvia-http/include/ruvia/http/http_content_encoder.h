#pragma once

#include <cstddef>
#include <limits>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"

namespace ruvia {

// PMR memory resource wrapper that enforces an exact live byte budget across
// all allocations (codec working memory and output buffers alike), throwing
// std::bad_alloc when an allocation would exceed max_bytes().
class http_memory_budget_resource final : public std::pmr::memory_resource {
public:
    explicit http_memory_budget_resource(std::size_t max_bytes,
        std::pmr::memory_resource* upstream = nullptr) noexcept
        : upstream_(detail::http_pmr_resource_or_default(upstream)),
          max_bytes_(max_bytes) {}

    http_memory_budget_resource(const http_memory_budget_resource&) = delete;
    http_memory_budget_resource& operator=(const http_memory_budget_resource&) = delete;

    [[nodiscard]] constexpr std::size_t max_bytes() const noexcept {
        return max_bytes_;
    }
    [[nodiscard]] constexpr std::size_t used_bytes() const noexcept {
        return used_bytes_;
    }
    [[nodiscard]] constexpr std::size_t peak_bytes() const noexcept {
        return peak_bytes_;
    }
    [[nodiscard]] std::pmr::memory_resource* upstream() const noexcept {
        return upstream_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::size_t max_bytes_;
    std::size_t used_bytes_{0};
    std::size_t peak_bytes_{0};
};

struct http_content_encoder_options final {
    std::pmr::memory_resource* resource_{nullptr};
    std::size_t max_encoded_bytes_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
};

// Synchronous incremental representation encoder. Each write consumes all of
// its input and appends to caller-owned output; it does not retain either view.
// Input may borrow output. Compressed writes snapshot that input in the encoder's
// resource before an append can invalidate it.
// The PMR resource must outlive the stable-address encoder. Output can be cleared
// between calls and remains valid after encoder destruction.
// Empty input is valid, including repeated flushes; it does not end the stream.
// Codec failures throw http_content_codec_error; allocation exceptions retain
// their original type when the codec returns through its C API. Brotli builds
// that exit on internal OOM cannot be recovered by this boundary.
// Either failure is terminal because input/output may have
// progressed. Later operations throw std::logic_error. finish() is idempotent;
// writing after successful finish throws std::logic_error without changing output.
// reset() returns an encoder in any phase to a fresh active stream. zlib and
// Zstd reuse their working memory; Brotli has no native reset and rebuilds from
// the same resource, which may allocate. A reset failure leaves the encoder failed.
class http_content_encoder final {
public:
    http_content_encoder(http_content_coding coding, std::pmr::memory_resource* resource = nullptr);
    http_content_encoder(http_content_coding coding, http_content_encoder_options options);
    ~http_content_encoder();
    http_content_encoder(const http_content_encoder&) = delete;
    http_content_encoder& operator=(const http_content_encoder&) = delete;
    http_content_encoder(http_content_encoder&&) = delete;
    http_content_encoder& operator=(http_content_encoder&&) = delete;

    [[nodiscard]] http_content_coding coding() const noexcept {
        return coding_;
    }

    // flush emits pending bytes for low-latency response streams such as SSE.
    void write(std::string_view input, std::pmr::string& output, bool flush = false);
    void finish(std::pmr::string& output);
    void reset();

private:
    struct impl_type;
    enum class phase : unsigned char { active,
        finished,
        failed };
    http_content_coding coding_;
    std::pmr::memory_resource* resource_;
    impl_type* impl_{nullptr};
    phase phase_{phase::active};
};

class http_content_transcode_error final : public http_content_codec_error {
public:
    http_content_transcode_error(http_content_decode_error error, const char* message)
        : http_content_codec_error(message),
          error_(error) {}

    [[nodiscard]] constexpr http_content_decode_error decode_error() const noexcept {
        return error_;
    }

private:
    http_content_decode_error error_;
};

struct http_content_transcoder_options final {
    std::pmr::memory_resource* resource_{nullptr};
    std::size_t max_decoded_bytes_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t max_encoded_bytes_{(std::numeric_limits<std::size_t>::max)()};
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
};

// Streaming content transcoder (and multi-stage incremental decoder when
// target_coding is identity). Decodes source_codings in reverse wire order and
// encodes into target_coding, sharing the caller's PMR memory resource.
class http_content_transcoder final {
public:
    http_content_transcoder(http_content_coding source_coding, http_content_coding target_coding,
        std::pmr::memory_resource* resource = nullptr);
    http_content_transcoder(http_content_coding source_coding, http_content_coding target_coding,
        http_content_transcoder_options options);
    http_content_transcoder(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, std::pmr::memory_resource* resource = nullptr);
    http_content_transcoder(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_content_transcoder_options options);
    ~http_content_transcoder();
    http_content_transcoder(const http_content_transcoder&) = delete;
    http_content_transcoder& operator=(const http_content_transcoder&) = delete;
    http_content_transcoder(http_content_transcoder&&) = delete;
    http_content_transcoder& operator=(http_content_transcoder&&) = delete;

    [[nodiscard]] http_content_coding target_coding() const noexcept {
        return target_coding_;
    }

    void write(std::string_view input, std::pmr::string& output, bool flush = false);
    void finish(std::pmr::string& output);
    void reset();

private:
    struct impl_type;
    enum class phase : unsigned char { active,
        finished,
        failed };
    http_content_coding target_coding_;
    std::pmr::memory_resource* resource_;
    impl_type* impl_{nullptr};
    phase phase_{phase::active};
};

}  // namespace ruvia
