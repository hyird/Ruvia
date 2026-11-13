#include "ruvia/http/http_content_encoder.h"

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"
#include "ruvia/http/http_content_codec.h"

#include "coding/http_brotli_stream.h"
#include "coding/http_content_codec.h"
#include "coding/http_zlib_stream.h"
#include "coding/http_zstd_stream.h"
#include "coding/pmr_codec_slot.h"
#include "util/http_address_overlap.h"
namespace ruvia {

void* http_memory_budget_resource::do_allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes > max_bytes_ - used_bytes_) {
        throw std::bad_alloc();
    }
    void* const pointer = upstream_->allocate(bytes, alignment);
    used_bytes_ += bytes;
    if (used_bytes_ > peak_bytes_) {
        peak_bytes_ = used_bytes_;
    }
    return pointer;
}

void http_memory_budget_resource::do_deallocate(
    void* pointer, std::size_t bytes, std::size_t alignment) {
    upstream_->deallocate(pointer, bytes, alignment);
    used_bytes_ = bytes <= used_bytes_ ? used_bytes_ - bytes : 0;
}

namespace {

using encoder_stream = std::variant<detail::zlib_deflate_stream, detail::brotli_encoder_stream, detail::zstd_encoder_stream>;

void emplace_encoder_stream(std::optional<encoder_stream>& target, http_content_coding coding,
    std::pmr::memory_resource* resource) {
    switch (coding) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
            target.emplace(std::in_place_type<detail::zlib_deflate_stream>, resource,
                coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                    : detail::zlib_content_format::deflate);
            return;
        case http_content_coding::brotli:
            target.emplace(std::in_place_type<detail::brotli_encoder_stream>, resource,
                detail::brotli_stream_window_bits);
            return;
        case http_content_coding::zstd:
            target.emplace(std::in_place_type<detail::zstd_encoder_stream>, resource);
            return;
        case http_content_coding::identity:
            break;
    }
    throw std::invalid_argument("unsupported HTTP content coding");
}

[[nodiscard]] const char* initialization_failure(const detail::zlib_deflate_stream&) noexcept {
    return "failed to initialize zlib content encoder";
}

[[nodiscard]] const char* initialization_failure(const detail::brotli_encoder_stream&) noexcept {
    return "failed to initialize Brotli content encoder";
}

[[nodiscard]] const char* initialization_failure(const detail::zstd_encoder_stream&) noexcept {
    return "failed to initialize Zstd content encoder";
}

[[nodiscard]] bool encode_stream(detail::zlib_deflate_stream& deflater, std::string_view input,
    std::pmr::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const int operation = finish_value ? Z_FINISH : flush ? Z_SYNC_FLUSH
                                                          : Z_NO_FLUSH;
    return deflater.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::brotli_encoder_stream& encoder, std::string_view input,
    std::pmr::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? BROTLI_OPERATION_FINISH : flush ? BROTLI_OPERATION_FLUSH
                                                                          : BROTLI_OPERATION_PROCESS;
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::zstd_encoder_stream& encoder, std::string_view input,
    std::pmr::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? ZSTD_e_end : flush ? ZSTD_e_flush
                                                             : ZSTD_e_continue;
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

}  // namespace

// One pinned codec stream per non-identity encoder. A failed initialization
// unwinds through the stream destructor, including an exception captured at
// the C ABI.
struct http_content_encoder::impl_type final {
    impl_type(http_content_coding coding, http_content_encoder_options options,
        std::pmr::memory_resource* upstream_resource)
        : budget_(options.max_memory_bytes_, upstream_resource),
          effective_resource_(options.max_memory_bytes_ == (std::numeric_limits<std::size_t>::max)()
                                  ? upstream_resource
                                  : &budget_),
          max_encoded_bytes_(options.max_encoded_bytes_),
          coding_(coding) {
        if (coding_ != http_content_coding::identity) {
            emplace_encoder_stream(stream_, coding_, effective_resource_);
            std::visit(
                [](const auto& stream) {
                    if (!stream.initialized()) {
                        throw http_content_codec_error(initialization_failure(stream));
                    }
                },
                *stream_);
        }
    }

    void encode(std::string_view input, std::pmr::string& output, bool flush, bool finish_value) {
        const auto before_size = output.size();
        if (!stream_.has_value()) {
            if (!input.empty()) {
                if (input.size() > max_encoded_bytes_ - produced_bytes_) {
                    throw http_content_codec_error("encoded HTTP content size exceeded limit");
                }
                output.append(input.data(), input.size());
                produced_bytes_ += input.size();
            }
            return;
        }
        const auto remaining = max_encoded_bytes_ - produced_bytes_;
        const auto output_cap = remaining > (std::numeric_limits<std::size_t>::max)() - before_size
                                    ? (std::numeric_limits<std::size_t>::max)()
                                    : before_size + remaining;
        const bool ok = std::visit(
            [&](auto& stream) {
                return encode_stream(stream, input, output, flush, finish_value, output_cap);
            },
            *stream_);
        if (!ok) {
            throw http_content_codec_error("encoded HTTP content size exceeded limit");
        }
        produced_bytes_ += output.size() - before_size;
    }

    // Reuse native working memory where supported. Brotli rebuilds from the
    // same resource. A failed rebuild leaves the variant valueless, which the
    // owner never touches again until another explicit reset().
    void reset() {
        produced_bytes_ = 0;
        if (!stream_.has_value()) {
            return;
        }
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&*stream_)) {
            if (!deflater->reset()) {
                throw http_content_codec_error("failed to reset zlib content encoder");
            }
        } else if (auto* encoder = std::get_if<detail::zstd_encoder_stream>(&*stream_)) {
            if (!encoder->reset()) {
                throw http_content_codec_error("failed to reset Zstd content encoder");
            }
        } else {
            const auto& rebuilt = stream_->emplace<detail::brotli_encoder_stream>(
                effective_resource_, detail::brotli_stream_window_bits);
            if (!rebuilt.initialized()) {
                throw http_content_codec_error(initialization_failure(rebuilt));
            }
        }
    }

    http_memory_budget_resource budget_;
    std::pmr::memory_resource* effective_resource_;
    std::optional<encoder_stream> stream_;
    std::size_t max_encoded_bytes_;
    std::size_t produced_bytes_{0};
    http_content_coding coding_;
};

http_content_encoder::http_content_encoder(
    http_content_coding coding, std::pmr::memory_resource* resource)
    : http_content_encoder(coding, http_content_encoder_options{.resource_ = resource}) {}

http_content_encoder::http_content_encoder(
    http_content_coding coding, http_content_encoder_options options)
    : coding_(coding),
      resource_(detail::http_pmr_resource_or_default(options.resource_)) {
    if (coding_ != http_content_coding::identity ||
        options.max_encoded_bytes_ != (std::numeric_limits<std::size_t>::max)() ||
        options.max_memory_bytes_ != (std::numeric_limits<std::size_t>::max)()) {
        if (http_content_coding_token(coding_).empty()) {
            throw std::invalid_argument("unsupported HTTP content coding");
        }
        impl_ = detail::construct_http_pmr_object<impl_type>(
            resource_, coding_, options, resource_);
    }
}

http_content_encoder::~http_content_encoder() {
    detail::destroy_http_pmr_object(impl_, resource_);
}

void http_content_encoder::write(std::string_view input, std::pmr::string& output, bool flush) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP content encoder is no longer writable");
    }
    try {
        if (impl_ == nullptr) {
            if (!input.empty()) {
                output.append(input.data(), input.size());
            }
        } else if (!input.empty() || flush) {
            if (detail::http_text_aliases_string(input, output)) {
                // Appending codec output can reallocate the caller's string
                // before all input has been consumed. Own only this aliasing
                // case; independent input keeps the direct encoding path.
                const std::pmr::string owned_input(input, impl_->effective_resource_);
                impl_->encode(owned_input, output, flush, false);
            } else {
                impl_->encode(input, output, flush, false);
            }
        }
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_encoder::finish(std::pmr::string& output) {
    if (phase_ == phase::finished) {
        return;
    }
    if (phase_ == phase::failed) {
        throw std::logic_error("HTTP content encoder has failed");
    }
    try {
        if (impl_ != nullptr) {
            impl_->encode({}, output, false, true);
        }
        phase_ = phase::finished;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_encoder::reset() {
    if (impl_ != nullptr) {
        try {
            impl_->reset();
        } catch (...) {
            phase_ = phase::failed;
            throw;
        }
    }
    phase_ = phase::active;
}
// The kept state of a resettable coding. The native libraries retain its
// address, so it lives in one stable allocation from the context resource.
struct http_content_encode_context::impl_type final {
    using stream_type = std::variant<detail::zlib_deflate_stream, detail::zstd_encoder_stream>;

    impl_type(http_content_coding coding, std::pmr::memory_resource* resource)
        : stream_(make_stream(coding, resource)) {}

    [[nodiscard]] bool initialized() const noexcept {
        return std::visit([](const auto& stream) { return stream.initialized(); }, stream_);
    }

    [[nodiscard]] bool reset() noexcept {
        return std::visit([](auto& stream) { return stream.reset(); }, stream_);
    }

    [[nodiscard]] std::optional<std::pmr::string> encode(
        std::string_view input, http_content_encode_options options) {
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&stream_)) {
            return detail::encode_zlib_content(*deflater, input, options.max_encoded_bytes_, options.resource_);
        }
        return detail::encode_zstd_content(
            std::get<detail::zstd_encoder_stream>(stream_), input, options.max_encoded_bytes_, options.resource_);
    }

    stream_type stream_;

private:
    [[nodiscard]] static stream_type make_stream(http_content_coding coding, std::pmr::memory_resource* resource) {
        if (coding == http_content_coding::zstd) {
            return stream_type(std::in_place_type<detail::zstd_encoder_stream>, resource);
        }
        return stream_type(std::in_place_type<detail::zlib_deflate_stream>, resource,
            coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                : detail::zlib_content_format::deflate);
    }
};

http_content_encode_context::http_content_encode_context(
    http_content_coding coding, std::pmr::memory_resource* state_resource) noexcept
    : coding_(coding),
      resource_(detail::http_pmr_resource_or_default(state_resource)) {}

http_content_encode_context::~http_content_encode_context() {
    detail::destroy_http_pmr_object(detail::http_resolved_pmr_resource_tag{}, impl_, resource_);
}

std::optional<std::pmr::string> http_content_encode_context::encode(
    std::string_view input, http_content_encode_options options) {
    switch (coding_) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
        case http_content_coding::zstd:
            break;
        case http_content_coding::brotli: {
            detail::brotli_encoder_stream encoder(resource_, detail::brotli_whole_buffer_window_bits);
            if (!encoder.initialized()) {
                throw http_content_codec_error("brotli encoder initialization failed");
            }
            return detail::encode_brotli_content(encoder, input, options.max_encoded_bytes_, options.resource_);
        }
        default:
            // identity and unknown values keep the one-shot facade's contract.
            return encode_http_content(coding_, input, options);
    }

    if (detail::acquire_pmr_codec(impl_, resource_, coding_, resource_) == nullptr) {
        throw http_content_codec_error("content encoder initialization failed");
    }
    const detail::pmr_codec_reset_guard reset_guard(impl_, resource_);
    return impl_->encode(input, options);
}

namespace {

class streaming_decode_stage final {
public:
    streaming_decode_stage(http_content_coding coding, std::pmr::memory_resource* resource,
        std::size_t max_decoded_bytes)
        : coding_(coding),
          max_decoded_bytes_(max_decoded_bytes) {
        emplace_decoder_stream(coding, resource, max_decoded_bytes);
    }
    streaming_decode_stage(const streaming_decode_stage&) = delete;
    streaming_decode_stage& operator=(const streaming_decode_stage&) = delete;

    void write(std::string_view input, std::pmr::string& output) {
        if (input.empty()) {
            return;
        }
        saw_input_ = true;
        if (auto* zlib_decoder = std::get_if<detail::zlib_inflate_stream>(&*stream_)) {
            write_zlib(*zlib_decoder, input, output);
        } else if (auto* brotli_decoder = std::get_if<detail::brotli_decoder_stream>(&*stream_)) {
            write_brotli(*brotli_decoder, input, output);
        } else if (auto* zstd_decoder = std::get_if<detail::zstd_decoder_stream>(&*stream_)) {
            write_zstd(*zstd_decoder, input, output);
        }
    }

    void finish() const {
        if (saw_input_ && !at_frame_end_) {
            throw http_content_transcode_error(
                http_content_decode_error::invalid_content, "truncated compressed HTTP content");
        }
    }

private:
    using decoder_variant = std::variant<detail::zlib_inflate_stream, detail::brotli_decoder_stream,
        detail::zstd_decoder_stream>;

    void emplace_decoder_stream(http_content_coding coding,
        std::pmr::memory_resource* resource, std::size_t max_decoded_bytes) {
        switch (coding) {
            case http_content_coding::gzip:
            case http_content_coding::deflate: {
                auto& var = stream_.emplace(std::in_place_type<detail::zlib_inflate_stream>,
                    resource,
                    coding == http_content_coding::gzip ? detail::zlib_gzip_window_bits
                                                        : detail::zlib_deflate_window_bits);
                if (!std::get<detail::zlib_inflate_stream>(var).initialized()) {
                    throw http_content_codec_error("failed to initialize zlib content decoder");
                }
                return;
            }
            case http_content_coding::brotli: {
                auto& var = stream_.emplace(std::in_place_type<detail::brotli_decoder_stream>,
                    resource, max_decoded_bytes);
                const auto& decoder = std::get<detail::brotli_decoder_stream>(var);
                if (!decoder.initialized()) {
                    if (decoder.native_budget_exceeded()) {
                        throw http_content_transcode_error(
                            http_content_decode_error::decoded_size_exceeded,
                            "Brotli decoder memory budget exceeded");
                    }
                    throw http_content_codec_error("failed to initialize Brotli content decoder");
                }
                return;
            }
            case http_content_coding::zstd: {
                auto& var =
                    stream_.emplace(std::in_place_type<detail::zstd_decoder_stream>, resource);
                if (!std::get<detail::zstd_decoder_stream>(var).initialized()) {
                    throw http_content_codec_error("failed to initialize Zstd content decoder");
                }
                return;
            }
            case http_content_coding::identity:
                break;
        }
        throw std::invalid_argument("unsupported HTTP content coding for decode stage");
    }

    void append_checked(std::pmr::string& output, const char* data, std::size_t size) {
        if (size > max_decoded_bytes_ - decoded_bytes_) {
            throw http_content_transcode_error(
                http_content_decode_error::decoded_size_exceeded,
                "decoded HTTP content size exceeded limit");
        }
        output.append(data, size);
        decoded_bytes_ += size;
    }

    void write_zlib(
        detail::zlib_inflate_stream& inflater, std::string_view input, std::pmr::string& output) {
        const bool allow_concatenated = coding_ == http_content_coding::gzip;
        const int window_bits =
            allow_concatenated ? detail::zlib_gzip_window_bits : detail::zlib_deflate_window_bits;
        auto& stream = inflater.native();
        const detail::zlib_borrow_scope borrow(stream);
        if (at_frame_end_) {
            if (!allow_concatenated) {
                throw http_content_transcode_error(http_content_decode_error::invalid_content,
                    "trailing bytes after deflate stream");
            }
            if (inflateReset2(&stream, window_bits) != Z_OK) {
                throw http_content_codec_error("failed to reset zlib content decoder");
            }
            at_frame_end_ = false;
        }
        std::size_t supplied = 0;
        std::array<char, 16384> buffer;
        for (;;) {
            detail::zlib_refill_input(stream, input, supplied);
            const auto before_input = stream.avail_in;
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            const int status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END) {
                inflater.rethrow_allocation_failure();
            }
            const auto produced = buffer.size() - stream.avail_out;
            if (produced != 0) {
                append_checked(output, buffer.data(), produced);
            }
            if (status == Z_STREAM_END) {
                at_frame_end_ = true;
                detail::zlib_refill_input(stream, input, supplied);
                if (stream.avail_in == 0 && supplied == input.size()) {
                    return;
                }
                if (!allow_concatenated) {
                    throw http_content_transcode_error(http_content_decode_error::invalid_content,
                        "trailing bytes after deflate stream");
                }
                auto* const next_input = stream.next_in;
                const auto available_input = stream.avail_in;
                if (inflateReset2(&stream, window_bits) != Z_OK) {
                    throw http_content_codec_error("failed to reset zlib content decoder");
                }
                stream.next_in = next_input;
                stream.avail_in = available_input;
                at_frame_end_ = false;
                continue;
            }
            if (status == Z_MEM_ERROR) {
                throw http_content_codec_error("zlib decoder ran out of memory");
            }
            if (status != Z_OK && status != Z_BUF_ERROR) {
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid zlib compressed content");
            }
            const bool progressed = produced != 0 || stream.avail_in != before_input;
            if (!progressed) {
                if (stream.avail_in == 0 && supplied < input.size()) {
                    continue;
                }
                if (stream.avail_in == 0 && supplied == input.size()) {
                    return;
                }
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid zlib compressed content");
            }
        }
    }

    void write_brotli(
        detail::brotli_decoder_stream& decoder, std::string_view input, std::pmr::string& output) {
        if (at_frame_end_) {
            throw http_content_transcode_error(
                http_content_decode_error::invalid_content, "trailing bytes after Brotli stream");
        }
        auto* const state_value = decoder.native();
        const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
        std::size_t available_input = input.size();
        for (;;) {
            const auto before_input = available_input;
            std::size_t available_output = 0;
            const auto result_value = BrotliDecoderDecompressStream(
                state_value, &available_input, &next_input, &available_output, nullptr, nullptr);
            bool produced_output = false;
            while (BrotliDecoderHasMoreOutput(state_value) == BROTLI_TRUE) {
                std::size_t produced = 0;
                const auto* const bytes_value = BrotliDecoderTakeOutput(state_value, &produced);
                if (produced != 0) {
                    append_checked(output, reinterpret_cast<const char*>(bytes_value), produced);
                    produced_output = true;
                }
            }
            if (result_value == BROTLI_DECODER_RESULT_SUCCESS) {
                if (available_input != 0) {
                    throw http_content_transcode_error(http_content_decode_error::invalid_content,
                        "trailing bytes after Brotli stream");
                }
                at_frame_end_ = true;
                return;
            }
            if (result_value == BROTLI_DECODER_RESULT_ERROR) {
                if (decoder.native_budget_exceeded()) {
                    throw http_content_transcode_error(
                        http_content_decode_error::decoded_size_exceeded,
                        "Brotli decoder memory budget exceeded");
                }
                decoder.rethrow_allocation_failure();
                throw http_content_transcode_error(http_content_decode_error::invalid_content,
                    "invalid Brotli compressed content");
            }
            if (result_value == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_input == 0) {
                return;
            }
            if (!produced_output && available_input == before_input) {
                throw http_content_transcode_error(http_content_decode_error::invalid_content,
                    "invalid Brotli compressed content");
            }
        }
    }

    void write_zstd(
        detail::zstd_decoder_stream& decoder, std::string_view input, std::pmr::string& output) {
        auto* const stream = decoder.native();
        ZSTD_inBuffer in{input.data(), input.size(), 0};
        std::array<char, 16384> buffer;
        at_frame_end_ = false;
        for (;;) {
            const auto before_input = in.pos;
            ZSTD_outBuffer out{buffer.data(), buffer.size(), 0};
            const auto result_value = ZSTD_decompressStream(stream, &out, &in);
            if (ZSTD_isError(result_value) != 0) {
                decoder.rethrow_allocation_failure();
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid Zstd compressed content");
            }
            if (out.pos != 0) {
                append_checked(output, buffer.data(), out.pos);
            }
            if (result_value == 0) {
                at_frame_end_ = true;
                if (in.pos == in.size) {
                    return;
                }
                at_frame_end_ = false;
                continue;
            }
            if (in.pos == in.size && out.pos < buffer.size()) {
                return;
            }
            if (out.pos == 0 && in.pos == before_input) {
                throw http_content_transcode_error(
                    http_content_decode_error::invalid_content, "invalid Zstd compressed content");
            }
        }
    }

    http_content_coding coding_;
    std::size_t max_decoded_bytes_;
    std::size_t decoded_bytes_{0};
    bool saw_input_{false};
    bool at_frame_end_{false};
    std::optional<decoder_variant> stream_;
};

}  // namespace

struct http_content_transcoder::impl_type final {
    using stage_ptr =
        std::unique_ptr<streaming_decode_stage, detail::http_pmr_object_deleter<streaming_decode_stage>>;

    impl_type(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_content_transcoder_options options,
        std::pmr::memory_resource* upstream_resource)
        : budget_(options.max_memory_bytes_, upstream_resource),
          effective_resource_(options.max_memory_bytes_ == (std::numeric_limits<std::size_t>::max)()
                                  ? upstream_resource
                                  : &budget_),
          source_codings_(effective_resource_),
          stages_(effective_resource_),
          scratch_a_(effective_resource_),
          scratch_b_(effective_resource_),
          options_(options),
          target_coding_(target_coding),
          encoder_(target_coding,
              http_content_encoder_options{
                  .resource_ = effective_resource_,
                  .max_encoded_bytes_ = options.max_encoded_bytes_,
                  .max_memory_bytes_ = (std::numeric_limits<std::size_t>::max)()}) {
        if (http_content_coding_token(target_coding).empty()) {
            throw std::invalid_argument("unsupported target HTTP content coding");
        }
        for (auto it = source_codings.rbegin(); it != source_codings.rend(); ++it) {
            if (http_content_coding_token(*it).empty()) {
                throw std::invalid_argument("unsupported source HTTP content coding");
            }
            if (*it != http_content_coding::identity) {
                source_codings_.push_back(*it);
            }
        }
        build_stages();
    }

    void build_stages() {
        stages_.clear();
        stages_.reserve(source_codings_.size());
        for (const auto coding : source_codings_) {
            stages_.push_back(detail::make_http_pmr_object<streaming_decode_stage>(
                effective_resource_, coding, effective_resource_, options_.max_decoded_bytes_));
        }
        identity_decoded_bytes_ = 0;
    }

    void write(std::string_view input, std::pmr::string& output, bool flush) {
        if (stages_.empty()) {
            if (input.size() > options_.max_decoded_bytes_ - identity_decoded_bytes_) {
                throw http_content_transcode_error(
                    http_content_decode_error::decoded_size_exceeded,
                    "decoded HTTP content size exceeded limit");
            }
            identity_decoded_bytes_ += input.size();
            encoder_.write(input, output, flush);
            return;
        }
        if (stages_.size() == 1 && target_coding_ == http_content_coding::identity &&
            options_.max_encoded_bytes_ == (std::numeric_limits<std::size_t>::max)() &&
            !detail::http_text_aliases_string(input, output)) {
            stages_.front()->write(input, output);
            return;
        }
        scratch_a_.clear();
        stages_.front()->write(input, scratch_a_);
        std::pmr::string* current = &scratch_a_;
        std::pmr::string* next = &scratch_b_;
        for (std::size_t index = 1; index < stages_.size(); ++index) {
            next->clear();
            stages_[index]->write(*current, *next);
            std::swap(current, next);
        }
        if (!current->empty() || flush) {
            encoder_.write(*current, output, flush);
        }
    }

    void finish(std::pmr::string& output) {
        for (const auto& stage : stages_) {
            stage->finish();
        }
        encoder_.finish(output);
    }

    void reset() {
        build_stages();
        scratch_a_.clear();
        scratch_b_.clear();
        encoder_.reset();
    }

    http_memory_budget_resource budget_;
    std::pmr::memory_resource* effective_resource_;
    std::pmr::vector<http_content_coding> source_codings_;
    std::pmr::vector<stage_ptr> stages_;
    std::pmr::string scratch_a_;
    std::pmr::string scratch_b_;
    http_content_transcoder_options options_;
    std::size_t identity_decoded_bytes_{0};
    http_content_coding target_coding_;
    http_content_encoder encoder_;
};

http_content_transcoder::http_content_transcoder(http_content_coding source_coding,
    http_content_coding target_coding, std::pmr::memory_resource* resource)
    : http_content_transcoder(std::span<const http_content_coding>(&source_coding, 1),
          target_coding, http_content_transcoder_options{.resource_ = resource}) {}

http_content_transcoder::http_content_transcoder(http_content_coding source_coding,
    http_content_coding target_coding, http_content_transcoder_options options)
    : http_content_transcoder(
          std::span<const http_content_coding>(&source_coding, 1), target_coding, options) {}

http_content_transcoder::http_content_transcoder(
    std::span<const http_content_coding> source_codings, http_content_coding target_coding,
    std::pmr::memory_resource* resource)
    : http_content_transcoder(
          source_codings, target_coding, http_content_transcoder_options{.resource_ = resource}) {}

http_content_transcoder::http_content_transcoder(
    std::span<const http_content_coding> source_codings, http_content_coding target_coding,
    http_content_transcoder_options options)
    : target_coding_(target_coding),
      resource_(detail::http_pmr_resource_or_default(options.resource_)),
      impl_(detail::construct_http_pmr_object<impl_type>(
          resource_, source_codings, target_coding, options, resource_)) {}

http_content_transcoder::~http_content_transcoder() {
    detail::destroy_http_pmr_object(impl_, resource_);
}

void http_content_transcoder::write(
    std::string_view input, std::pmr::string& output, bool flush) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP content transcoder is no longer writable");
    }
    try {
        impl_->write(input, output, flush);
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_transcoder::finish(std::pmr::string& output) {
    if (phase_ == phase::finished) {
        return;
    }
    if (phase_ == phase::failed) {
        throw std::logic_error("HTTP content transcoder has failed");
    }
    try {
        impl_->finish(output);
        phase_ = phase::finished;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_content_transcoder::reset() {
    try {
        impl_->reset();
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
    phase_ = phase::active;
}

http_worker_codecs::http_worker_codecs(std::pmr::memory_resource* resource) noexcept
    : resource_(detail::http_pmr_resource_or_default(resource)),
      gzip_(http_content_coding::gzip, resource_),
      deflate_(http_content_coding::deflate, resource_),
      brotli_(http_content_coding::brotli, resource_),
      zstd_(http_content_coding::zstd, resource_) {}

http_worker_codecs::~http_worker_codecs() {
    for (auto* pool : {&gzip_.streaming_, &deflate_.streaming_, &brotli_.streaming_, &zstd_.streaming_}) {
        for (std::size_t i = 0; i < pool->count_; ++i) {
            destroy_stream_encoder(*pool->free_[i]);
        }
        pool->count_ = 0;
    }
}

http_content_encode_context& http_worker_codecs::encoder(http_content_coding coding) {
    return encoders(coding).buffered_;
}

http_worker_codecs::coding_encoders_type& http_worker_codecs::encoders(http_content_coding coding) {
    switch (coding) {
        case http_content_coding::gzip:
            return gzip_;
        case http_content_coding::deflate:
            return deflate_;
        case http_content_coding::brotli:
            return brotli_;
        case http_content_coding::zstd:
            return zstd_;
        case http_content_coding::identity:
            break;
    }
    throw std::logic_error("identity content coding has no encoder");
}

http_stream_encoder_lease http_worker_codecs::acquire_stream_encoder(http_content_coding coding) {
    auto& pool = encoders(coding).streaming_;
    if (pool.count_ != 0) {
        return http_stream_encoder_lease(*this, *pool.free_[--pool.count_]);
    }
    return http_stream_encoder_lease(
        *this, *detail::construct_http_pmr_object<http_content_encoder>(
                   resource_, coding, resource_));
}

void http_worker_codecs::release_stream_encoder(http_content_encoder& encoder) noexcept {
    auto& pool = encoders(encoder.coding()).streaming_;
    if (pool.count_ < pool.free_.size()) {
        try {
            encoder.reset();
            pool.free_[pool.count_++] = &encoder;
            return;
        } catch (...) {
        }
    }
    destroy_stream_encoder(encoder);
}

void http_worker_codecs::destroy_stream_encoder(http_content_encoder& encoder) noexcept {
    detail::destroy_http_pmr_object(&encoder, resource_);
}

}  // namespace ruvia
