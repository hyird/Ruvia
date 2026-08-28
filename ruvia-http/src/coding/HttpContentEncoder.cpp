#include "ruvia/http/detail/coding/HttpContentEncoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include <brotli/encode.h>
#include <zlib.h>
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#undef ZSTD_STATIC_LINKING_ONLY

#include "ruvia/http/detail/coding/PmrCodecAllocation.h"
#include "ruvia/http/detail/coding/ZlibPmrAllocation.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia::detail {
namespace {

voidpf gzipAllocate(voidpf opaque, uInt items, uInt size) noexcept {
    return zlibPmrAllocate(static_cast<std::pmr::memory_resource*>(opaque), items, size);
}

void gzipFree(voidpf, voidpf address) noexcept {
    zlibPmrFree(address);
}

[[nodiscard]] bool appendOutput(std::pmr::string& output, const char* bytes, std::size_t size) noexcept {
    try {
        output.append(bytes, size);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

struct HttpContentEncoder::Impl final {
    z_stream gzip{};
    BrotliEncoderState* brotli{nullptr};
    ZSTD_CCtx* zstd{nullptr};
};

namespace {

void destroyEncoderState(HttpContentCoding coding, HttpContentEncoder::Impl& impl) noexcept {
    switch (coding) {
        case HttpContentCoding::kGzip:
            if (impl.gzip.state != nullptr) {
                (void)deflateEnd(&impl.gzip);
            }
            break;
        case HttpContentCoding::kBrotli:
            if (impl.brotli != nullptr) {
                BrotliEncoderDestroyInstance(impl.brotli);
            }
            break;
        case HttpContentCoding::kZstd:
            if (impl.zstd != nullptr) {
                (void)ZSTD_freeCCtx(impl.zstd);
            }
            break;
        case HttpContentCoding::kIdentity:
            break;
    }
}

[[nodiscard]] bool initializeEncoderState(HttpContentCoding coding, HttpContentEncoder::Impl& impl, std::pmr::memory_resource* resource) {
    switch (coding) {
        case HttpContentCoding::kIdentity:
            return true;
        case HttpContentCoding::kGzip:
            impl.gzip.zalloc = &gzipAllocate;
            impl.gzip.zfree = &gzipFree;
            impl.gzip.opaque = resource;
            return deflateInit2(&impl.gzip, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK;
        case HttpContentCoding::kBrotli:
            impl.brotli = BrotliEncoderCreateInstance(&pmrCodecAllocate, &pmrCodecFree, resource);
            return impl.brotli != nullptr && BrotliEncoderSetParameter(impl.brotli, BROTLI_PARAM_QUALITY, 5) == BROTLI_TRUE;
        case HttpContentCoding::kZstd: {
            impl.zstd = ZSTD_createCCtx_advanced(ZSTD_customMem{&pmrCodecAllocate, &pmrCodecFree, resource});
            return impl.zstd != nullptr && ZSTD_isError(ZSTD_CCtx_setParameter(impl.zstd, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT)) == 0 && ZSTD_isError(ZSTD_CCtx_setParameter(impl.zstd, ZSTD_c_windowLog, 23)) == 0;
        }
    }
    return false;
}

[[nodiscard]] HttpContentEncodeStep encodeGzip(HttpContentEncoder::Impl& impl, std::string_view input, std::pmr::string& output, bool flush) {
    std::size_t supplied = 0;
    const auto operation = flush ? Z_SYNC_FLUSH : Z_NO_FLUSH;
    std::array<char, 16384> buffer{};
    for (;;) {
        if (impl.gzip.avail_in == 0 && supplied < input.size()) {
            const auto count = static_cast<uInt>(std::min<std::size_t>(input.size() - supplied, (std::numeric_limits<uInt>::max)()));
            impl.gzip.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data() + supplied));
            impl.gzip.avail_in = count;
            supplied += count;
        }
        const auto beforeInput = impl.gzip.avail_in;
        impl.gzip.next_out = reinterpret_cast<Bytef*>(buffer.data());
        impl.gzip.avail_out = static_cast<uInt>(buffer.size());
        const auto status = deflate(&impl.gzip, operation);
        const auto produced = buffer.size() - impl.gzip.avail_out;
        if (!appendOutput(output, buffer.data(), produced)) {
            return HttpContentEncodeStep::kFailure;
        }
        if (status != Z_OK) {
            return HttpContentEncodeStep::kFailure;
        }
        if (impl.gzip.avail_in == 0 && supplied == input.size() && impl.gzip.avail_out != 0) {
            return HttpContentEncodeStep::kProducedOrPending;
        }
        if (produced == 0 && impl.gzip.avail_in == beforeInput) {
            return HttpContentEncodeStep::kFailure;
        }
    }
}

[[nodiscard]] HttpContentEncodeStep finishGzip(HttpContentEncoder::Impl& impl, std::pmr::string& output) {
    std::array<char, 16384> buffer{};
    for (;;) {
        impl.gzip.next_in = nullptr;
        impl.gzip.avail_in = 0;
        impl.gzip.next_out = reinterpret_cast<Bytef*>(buffer.data());
        impl.gzip.avail_out = static_cast<uInt>(buffer.size());
        const auto status = deflate(&impl.gzip, Z_FINISH);
        const auto produced = buffer.size() - impl.gzip.avail_out;
        if (!appendOutput(output, buffer.data(), produced)) {
            return HttpContentEncodeStep::kFailure;
        }
        if (status == Z_STREAM_END) {
            return HttpContentEncodeStep::kFinished;
        }
        if (status != Z_OK || produced == 0) {
            return HttpContentEncodeStep::kFailure;
        }
    }
}

[[nodiscard]] HttpContentEncodeStep encodeBrotli(HttpContentEncoder::Impl& impl, std::string_view input, std::pmr::string& output, BrotliEncoderOperation operation) {
    std::size_t availableInput = input.size();
    const auto* nextInput = reinterpret_cast<const std::uint8_t*>(input.data());
    std::array<std::uint8_t, 16384> buffer{};
    for (;;) {
        std::size_t availableOutput = buffer.size();
        auto* nextOutput = buffer.data();
        const auto beforeInput = availableInput;
        if (BrotliEncoderCompressStream(impl.brotli, operation, &availableInput, &nextInput, &availableOutput, &nextOutput, nullptr) != BROTLI_TRUE) {
            return HttpContentEncodeStep::kFailure;
        }
        const auto produced = buffer.size() - availableOutput;
        if (!appendOutput(output, reinterpret_cast<const char*>(buffer.data()), produced)) {
            return HttpContentEncodeStep::kFailure;
        }
        const bool pending = BrotliEncoderHasMoreOutput(impl.brotli) == BROTLI_TRUE;
        if (operation == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(impl.brotli) == BROTLI_TRUE) {
            return HttpContentEncodeStep::kFinished;
        }
        if (availableInput == 0 && !pending) {
            return HttpContentEncodeStep::kProducedOrPending;
        }
        if (produced == 0 && availableInput == beforeInput) {
            return HttpContentEncodeStep::kFailure;
        }
    }
}

[[nodiscard]] HttpContentEncodeStep encodeZstd(HttpContentEncoder::Impl& impl, std::string_view input, std::pmr::string& output, ZSTD_EndDirective operation) {
    ZSTD_inBuffer inputBuffer{input.data(), input.size(), 0};
    std::array<char, 16384> buffer{};
    for (;;) {
        ZSTD_outBuffer outputBuffer{buffer.data(), buffer.size(), 0};
        const auto beforeInput = inputBuffer.pos;
        const auto remaining = ZSTD_compressStream2(impl.zstd, &outputBuffer, &inputBuffer, operation);
        if (ZSTD_isError(remaining) != 0) {
            return HttpContentEncodeStep::kFailure;
        }
        if (!appendOutput(output, buffer.data(), outputBuffer.pos)) {
            return HttpContentEncodeStep::kFailure;
        }
        if (operation == ZSTD_e_end && remaining == 0 && inputBuffer.pos == inputBuffer.size) {
            return HttpContentEncodeStep::kFinished;
        }
        if (operation != ZSTD_e_end && inputBuffer.pos == inputBuffer.size && outputBuffer.pos < buffer.size() && (operation == ZSTD_e_continue || remaining == 0)) {
            return HttpContentEncodeStep::kProducedOrPending;
        }
        if (outputBuffer.pos == 0 && inputBuffer.pos == beforeInput) {
            return HttpContentEncodeStep::kFailure;
        }
    }
}

}  // namespace

HttpContentEncoder::HttpContentEncoder(HttpContentCoding coding, std::pmr::memory_resource* resource)
    : coding_(coding),
      resource_(httpPmrResourceOrDefault(resource)),
      impl_(nullptr) {
    if (coding_ == HttpContentCoding::kIdentity) {
        return;
    }
    std::pmr::polymorphic_allocator<Impl> allocator(resource_);
    impl_ = allocator.allocate(1);
    try {
        std::construct_at(impl_);
        if (!initializeEncoderState(coding_, *impl_, resource_)) {
            destroyEncoderState(coding_, *impl_);
            std::destroy_at(impl_);
            allocator.deallocate(impl_, 1);
            impl_ = nullptr;
            throw std::runtime_error("failed to initialize HTTP content encoder");
        }
    } catch (...) {
        if (impl_ != nullptr) {
            allocator.deallocate(impl_, 1);
            impl_ = nullptr;
        }
        throw;
    }
}

HttpContentEncoder::~HttpContentEncoder() {
    if (impl_ == nullptr) {
        return;
    }
    destroyEncoderState(coding_, *impl_);
    std::destroy_at(impl_);
    std::pmr::polymorphic_allocator<Impl> allocator(resource_);
    allocator.deallocate(impl_, 1);
}

HttpContentEncodeStep HttpContentEncoder::write(std::string_view input, std::pmr::string& output, bool flush) {
    if (finished_ || failed_) {
        return HttpContentEncodeStep::kFailure;
    }
    if (impl_ == nullptr) {
        if (input.empty()) {
            return HttpContentEncodeStep::kProducedOrPending;
        }
        try {
            output.append(input.data(), input.size());
            return HttpContentEncodeStep::kProducedOrPending;
        } catch (...) {
            failed_ = true;
            return HttpContentEncodeStep::kFailure;
        }
    }
    if (input.empty() && !flush) {
        return HttpContentEncodeStep::kProducedOrPending;
    }
    HttpContentEncodeStep result = HttpContentEncodeStep::kFailure;
    switch (coding_) {
        case HttpContentCoding::kGzip:
            result = encodeGzip(*impl_, input, output, flush);
            break;
        case HttpContentCoding::kBrotli:
            result = encodeBrotli(*impl_, input, output, flush ? BROTLI_OPERATION_FLUSH : BROTLI_OPERATION_PROCESS);
            break;
        case HttpContentCoding::kZstd:
            result = encodeZstd(*impl_, input, output, flush ? ZSTD_e_flush : ZSTD_e_continue);
            break;
        case HttpContentCoding::kIdentity:
            break;
    }
    if (result == HttpContentEncodeStep::kFailure) {
        failed_ = true;
    }
    return result;
}

HttpContentEncodeStep HttpContentEncoder::finish(std::pmr::string& output) {
    if (finished_) {
        return HttpContentEncodeStep::kFinished;
    }
    if (failed_) {
        return HttpContentEncodeStep::kFailure;
    }
    if (impl_ == nullptr) {
        finished_ = true;
        return HttpContentEncodeStep::kFinished;
    }
    HttpContentEncodeStep result = HttpContentEncodeStep::kFailure;
    switch (coding_) {
        case HttpContentCoding::kGzip:
            result = finishGzip(*impl_, output);
            break;
        case HttpContentCoding::kBrotli:
            result = encodeBrotli(*impl_, {}, output, BROTLI_OPERATION_FINISH);
            break;
        case HttpContentCoding::kZstd:
            result = encodeZstd(*impl_, {}, output, ZSTD_e_end);
            break;
        case HttpContentCoding::kIdentity:
            result = HttpContentEncodeStep::kFinished;
            break;
    }
    if (result == HttpContentEncodeStep::kFinished) {
        finished_ = true;
    } else if (result == HttpContentEncodeStep::kFailure) {
        failed_ = true;
    }
    return result;
}

}  // namespace ruvia::detail
