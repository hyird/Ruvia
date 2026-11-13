#pragma once

#include <array>
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

struct http_staged_transcode_options final {
    std::size_t min_bytes_{0};
    std::size_t max_bytes_{std::size_t{64} * 1024 * 1024};
    std::size_t max_memory_bytes_{(std::numeric_limits<std::size_t>::max)()};
    bool require_smaller_than_source_{false};
    std::pmr::memory_resource* resource_{nullptr};
};

class http_staged_transcode_result final {
public:
    http_staged_transcode_result(bool transformed, http_content_coding coding,
        std::size_t decoded_bytes, std::pmr::string bytes) noexcept
        : bytes_(std::move(bytes)),
          decoded_bytes_(decoded_bytes),
          coding_(coding),
          transformed_(transformed) {}

    http_staged_transcode_result(const http_staged_transcode_result&) = default;
    http_staged_transcode_result(http_staged_transcode_result&&) noexcept = default;
    http_staged_transcode_result& operator=(const http_staged_transcode_result&) = delete;
    http_staged_transcode_result& operator=(http_staged_transcode_result&&) = delete;

    [[nodiscard]] constexpr bool transformed() const noexcept {
        return transformed_;
    }
    [[nodiscard]] constexpr http_content_coding coding() const noexcept {
        return coding_;
    }
    [[nodiscard]] constexpr std::size_t decoded_bytes() const noexcept {
        return decoded_bytes_;
    }

    [[nodiscard]] std::string_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return bytes_;
    }
    std::string_view bytes() const&& = delete;

    [[nodiscard]] std::pmr::string take_bytes() && noexcept {
        return std::move(bytes_);
    }

private:
    std::pmr::string bytes_;
    std::size_t decoded_bytes_{0};
    http_content_coding coding_{http_content_coding::identity};
    bool transformed_{false};
};

// Size-range staging transcoder for responses without Content-Length (e.g.
// chunked or HTTP/2/3 streams). Stages raw and decoded bytes up to max_bytes_;
// if the final decoded length lies within [min_bytes_, max_bytes_], encodes
// into target_coding, otherwise falls back to the original untransformed stream.
// The two-argument write(input, passthrough_output) overload spills staged
// raw bytes directly into passthrough_output as soon as max_bytes_ is exceeded.
class http_staged_content_transcoder final {
public:
    http_staged_content_transcoder(http_content_coding source_coding,
        http_content_coding target_coding, http_staged_transcode_options options = {});
    http_staged_content_transcoder(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_staged_transcode_options options = {});
    ~http_staged_content_transcoder();
    http_staged_content_transcoder(const http_staged_content_transcoder&) = delete;
    http_staged_content_transcoder& operator=(const http_staged_content_transcoder&) = delete;
    http_staged_content_transcoder(http_staged_content_transcoder&&) = delete;
    http_staged_content_transcoder& operator=(http_staged_content_transcoder&&) = delete;

    [[nodiscard]] http_content_coding target_coding() const noexcept {
        return target_coding_;
    }
    [[nodiscard]] bool passthrough() const noexcept;
    [[nodiscard]] std::size_t staged_raw_bytes() const noexcept;
    [[nodiscard]] std::size_t decoded_bytes() const noexcept;

    // Returns true while still staging within max_bytes_, or false once
    // switched to passthrough fallback.
    bool write(std::string_view input);
    bool write(std::string_view input, std::pmr::string& passthrough_output);

    [[nodiscard]] http_staged_transcode_result finish();
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

class http_worker_codecs;

// RAII lease for a pooled incremental encoder from http_worker_codecs.
// Destroying a non-discarded lease resets and returns the encoder to the pool;
// discard() destroys a failed encoder instead of pooling it.
class http_stream_encoder_lease final {
public:
    http_stream_encoder_lease(const http_stream_encoder_lease&) = delete;
    http_stream_encoder_lease& operator=(const http_stream_encoder_lease&) = delete;
    http_stream_encoder_lease(http_stream_encoder_lease&& other) noexcept
        : codecs_(std::exchange(other.codecs_, nullptr)),
          encoder_(std::exchange(other.encoder_, nullptr)) {}
    ~http_stream_encoder_lease() {
        release();
    }

    [[nodiscard]] http_content_encoder& encoder() & noexcept RUVIA_LIFETIMEBOUND {
        return *encoder_;
    }
    http_content_encoder& encoder() && = delete;

    void discard() noexcept;

private:
    friend class http_worker_codecs;

    http_stream_encoder_lease(http_worker_codecs& codecs, http_content_encoder& encoder) noexcept
        : codecs_(&codecs),
          encoder_(&encoder) {}

    void release() noexcept;

    http_worker_codecs* codecs_{nullptr};
    http_content_encoder* encoder_{nullptr};
};

// Per-worker reusable buffered and streaming content encoder pool.
class http_worker_codecs final {
public:
    explicit http_worker_codecs(std::pmr::memory_resource* resource = nullptr) noexcept;
    ~http_worker_codecs();
    http_worker_codecs(const http_worker_codecs&) = delete;
    http_worker_codecs& operator=(const http_worker_codecs&) = delete;

    [[nodiscard]] http_content_encode_context& encoder(http_content_coding coding);
    [[nodiscard]] http_stream_encoder_lease acquire_stream_encoder(http_content_coding coding);

private:
    friend class http_stream_encoder_lease;

    static constexpr std::size_t max_pooled_stream_encoders = 8;

    struct stream_encoder_pool_type final {
        std::array<http_content_encoder*, max_pooled_stream_encoders> free_{};
        std::size_t count_{0};
    };

    struct coding_encoders_type final {
        coding_encoders_type(http_content_coding coding, std::pmr::memory_resource* resource) noexcept
            : buffered_(coding, resource) {}

        http_content_encode_context buffered_;
        stream_encoder_pool_type streaming_;
    };

    [[nodiscard]] coding_encoders_type& encoders(http_content_coding coding);
    void release_stream_encoder(http_content_encoder& encoder) noexcept;
    void destroy_stream_encoder(http_content_encoder& encoder) noexcept;

    std::pmr::memory_resource* resource_;
    coding_encoders_type gzip_;
    coding_encoders_type deflate_;
    coding_encoders_type brotli_;
    coding_encoders_type zstd_;
};

inline void http_stream_encoder_lease::release() noexcept {
    if (encoder_ != nullptr) {
        auto* const codecs = std::exchange(codecs_, nullptr);
        codecs->release_stream_encoder(*std::exchange(encoder_, nullptr));
    }
}

inline void http_stream_encoder_lease::discard() noexcept {
    if (encoder_ != nullptr) {
        auto* const codecs = std::exchange(codecs_, nullptr);
        codecs->destroy_stream_encoder(*std::exchange(encoder_, nullptr));
    }
}

}  // namespace ruvia
