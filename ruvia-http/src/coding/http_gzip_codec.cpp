#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <utility>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/http_content_codec.h"
#include "coding/http_zlib_stream.h"

// gzip (RFC 1952) and zlib-wrapped deflate through zlib, with zlib's allocator
// routed to the caller's memory resource so neither direction makes a global allocation.

namespace ruvia::detail {

static http_content_decode_result decode_zlib_content(std::string_view input,
    std::size_t max_decoded_bytes, std::pmr::memory_resource* resource, int window_bits,
    bool allow_concatenated_members) {
    std::pmr::string output(http_pmr_resource_or_default(resource));
    zlib_inflate_stream inflater(output.get_allocator().resource(), window_bits);
    if (!inflater.initialized()) {
        return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
    }
    auto& stream = inflater.native();

    std::size_t supplied = 0;
    char buffer[16384];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = static_cast<uInt>(sizeof(buffer));
        const int status = inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
            inflater.rethrow_allocation_failure();
        }
        const auto produced = sizeof(buffer) - stream.avail_out;
        if (!append_decoded_bytes(output, buffer, produced, max_decoded_bytes)) {
            return http_content_decode_result_access::failure(http_content_decode_error::decoded_size_exceeded);
        }

        if (status == Z_STREAM_END) {
            // RFC 1952 gzip data is a series of members. Preserve any input
            // already supplied to zlib, reset only the member state, and keep
            // decoding until the exact HTTP content boundary is consumed.
            zlib_refill_input(stream, input, supplied);
            if (stream.avail_in == 0 && supplied == input.size()) {
                return http_content_decode_result_access::decoded(std::move(output));
            }
            if (!allow_concatenated_members) {
                return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
            }
            auto* next_input = stream.next_in;
            const auto available_input = stream.avail_in;
            const int reset = inflateReset2(&stream, window_bits);
            if (reset != Z_OK) {
                inflater.rethrow_allocation_failure();
                return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
            }
            stream.next_in = next_input;
            stream.avail_in = available_input;
            continue;
        }
        if (status == Z_MEM_ERROR) {
            return http_content_decode_result_access::failure(http_content_decode_error::decoder_failure);
        }
        if (status != Z_OK && status != Z_BUF_ERROR) {
            return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
        }

        const bool progressed = produced != 0 || stream.avail_in != before_input;
        if (!progressed) {
            if (stream.avail_in == 0 && supplied < input.size()) {
                continue;
            }
            return http_content_decode_result_access::failure(http_content_decode_error::invalid_content);
        }
    }
}

http_content_encode_result encode_zlib_content(zlib_deflate_stream& deflater, std::string_view input,
    std::size_t max_encoded_bytes, std::pmr::memory_resource* output_resource) {
    std::pmr::string output(http_pmr_resource_or_default(output_resource));
    auto& stream = deflater.native();
    const zlib_borrow_scope borrow(stream);

    std::size_t supplied = 0;
    // Uninitialized scratch: only the bytes zlib produced are copied out.
    char buffer[8192];
    for (;;) {
        zlib_refill_input(stream, input, supplied);
        if (output.size() == max_encoded_bytes) {
            if (stream.avail_in == 0 && supplied == input.size()) {
                Bytef probe_value{};
                stream.next_out = &probe_value;
                stream.avail_out = 1;
                const auto status = deflate(&stream, Z_FINISH);
                if (status != Z_OK && status != Z_STREAM_END) {
                    deflater.rethrow_allocation_failure();
                }
                if (status == Z_STREAM_END && stream.avail_out == 1) {
                    return http_content_encode_result_access::encoded(std::move(output));
                }
                if (status == Z_MEM_ERROR) {
                    return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
                }
            }
            return http_content_encode_result_access::failure(http_content_encode_error::encoded_size_exceeded);
        }
        const auto offset = output.size();
        const auto writable = std::min<std::size_t>(sizeof(buffer), max_encoded_bytes - offset);
        const auto before_input = stream.avail_in;
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = static_cast<uInt>(writable);
        const int status = deflate(&stream, stream.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
        output.append(buffer, writable - stream.avail_out);
        if (status != Z_OK && status != Z_STREAM_END) {
            deflater.rethrow_allocation_failure();
        }
        if (status == Z_STREAM_END) {
            return http_content_encode_result_access::encoded(std::move(output));
        }
        if (status == Z_MEM_ERROR) {
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
        if (status != Z_OK || (output.size() == offset && stream.avail_in == before_input)) {
            return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
        }
    }
}

namespace {

[[nodiscard]] http_content_encode_result encode_zlib_content(std::string_view input,
    std::size_t max_encoded_bytes, std::pmr::memory_resource* resource, int window_bits) {
    auto* const state_resource = http_pmr_resource_or_default(resource);
    zlib_deflate_stream deflater(
        state_resource, Z_DEFAULT_COMPRESSION, window_bits, zlib_default_mem_level, Z_DEFAULT_STRATEGY);
    if (!deflater.initialized()) {
        return http_content_encode_result_access::failure(http_content_encode_error::encoder_failure);
    }
    return encode_zlib_content(deflater, input, max_encoded_bytes, state_resource);
}

}  // namespace

http_content_decode_result decode_gzip_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    return decode_zlib_content(input, max_decoded_bytes, resource, zlib_gzip_window_bits, true);
}

http_content_decode_result decode_deflate_content(
    std::string_view input, std::size_t max_decoded_bytes, std::pmr::memory_resource* resource) {
    return decode_zlib_content(input, max_decoded_bytes, resource, zlib_deflate_window_bits, false);
}

http_content_encode_result encode_gzip_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    return encode_zlib_content(input, max_encoded_bytes, resource, zlib_gzip_window_bits);
}

http_content_encode_result encode_deflate_content(
    std::string_view input, std::size_t max_encoded_bytes, std::pmr::memory_resource* resource) {
    return encode_zlib_content(input, max_encoded_bytes, resource, zlib_deflate_window_bits);
}

}  // namespace ruvia::detail
