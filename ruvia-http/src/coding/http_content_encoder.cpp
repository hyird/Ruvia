#include "ruvia/http/http_content_encoder.h"

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http_content_codec.h"

#include "coding/codec_slot.h"
#include "coding/http_brotli_stream.h"
#include "coding/http_content_codec.h"
#include "coding/http_zlib_stream.h"
#include "coding/http_zstd_stream.h"
#include "memory/http_address_overlap.h"
namespace ruvia {

namespace {

using encoder_stream = std::variant<detail::zlib_deflate_stream, detail::brotli_encoder_stream, detail::zstd_encoder_stream>;

void emplace_encoder_stream(std::optional<encoder_stream>& target, http_content_coding coding,
    std::size_t max_memory_bytes) {
    switch (coding) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
            target.emplace(std::in_place_type<detail::zlib_deflate_stream>,
                coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                    : detail::zlib_content_format::deflate,
                max_memory_bytes);
            return;
        case http_content_coding::brotli:
            target.emplace(std::in_place_type<detail::brotli_encoder_stream>,
                detail::brotli_stream_window_bits, max_memory_bytes);
            return;
        case http_content_coding::zstd:
            target.emplace(std::in_place_type<detail::zstd_encoder_stream>, max_memory_bytes);
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
    std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const int operation = finish_value ? Z_FINISH : flush ? Z_SYNC_FLUSH
                                                          : Z_NO_FLUSH;
    return deflater.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::brotli_encoder_stream& encoder, std::string_view input,
    std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? BROTLI_OPERATION_FINISH : flush ? BROTLI_OPERATION_FLUSH
                                                                          : BROTLI_OPERATION_PROCESS;
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

[[nodiscard]] bool encode_stream(detail::zstd_encoder_stream& encoder, std::string_view input,
    std::string& output, bool flush, bool finish_value, std::size_t max_encoded_bytes) {
    const auto operation = finish_value ? ZSTD_e_end : flush ? ZSTD_e_flush
                                                             : ZSTD_e_continue;
    return encoder.encode(input, output, operation, max_encoded_bytes);
}

}  // namespace

// One pinned codec stream per non-identity encoder. A failed initialization
// unwinds through the stream destructor, including an exception captured at
// the C ABI.
struct http_content_encoder::impl_type final {
    impl_type(http_content_coding coding, http_content_encoder_options options)
        : max_encoded_bytes_(options.max_encoded_bytes_),
          max_memory_bytes_(options.max_memory_bytes_),
          coding_(coding) {
        if (coding_ != http_content_coding::identity) {
            emplace_encoder_stream(stream_, coding_, max_memory_bytes_);
            std::visit(
                [](const auto& stream) {
                    if (!stream.initialized()) {
                        throw http_content_codec_error(initialization_failure(stream));
                    }
                },
                *stream_);
        }
    }

    void encode(std::string_view input, std::string& output, bool flush, bool finish_value) {
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

    // Reuse native working memory where supported. Brotli rebuilds.
    // A failed rebuild leaves the variant valueless, which the
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
                detail::brotli_stream_window_bits, max_memory_bytes_);
            if (!rebuilt.initialized()) {
                throw http_content_codec_error(initialization_failure(rebuilt));
            }
        }
    }

    std::optional<encoder_stream> stream_;
    std::size_t max_encoded_bytes_;
    std::size_t max_memory_bytes_;
    std::size_t produced_bytes_{0};
    http_content_coding coding_;
};

http_content_encoder::http_content_encoder(
    http_content_coding coding, http_content_encoder_options options)
    : coding_(coding) {
    if (coding_ != http_content_coding::identity ||
        options.max_encoded_bytes_ != (std::numeric_limits<std::size_t>::max)() ||
        options.max_memory_bytes_ != (std::numeric_limits<std::size_t>::max)()) {
        if (http_content_coding_token(coding_).empty()) {
            throw std::invalid_argument("unsupported HTTP content coding");
        }
        impl_ = new impl_type(coding_, options);
    }
}

http_content_encoder::~http_content_encoder() {
    delete impl_;
}

void http_content_encoder::write(std::string_view input, std::string& output, bool flush) {
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
                const std::string owned_input(input);
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

void http_content_encoder::finish(std::string& output) {
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
// address, so it lives in one stable allocation.
struct http_content_encode_context::impl_type final {
    using stream_type = std::variant<detail::zlib_deflate_stream, detail::zstd_encoder_stream>;

    explicit impl_type(http_content_coding coding)
        : stream_(make_stream(coding)) {}

    [[nodiscard]] bool initialized() const noexcept {
        return std::visit([](const auto& stream) { return stream.initialized(); }, stream_);
    }

    [[nodiscard]] bool reset() noexcept {
        return std::visit([](auto& stream) { return stream.reset(); }, stream_);
    }

    [[nodiscard]] std::optional<std::string> encode(
        std::string_view input, http_content_encode_options options) {
        if (auto* deflater = std::get_if<detail::zlib_deflate_stream>(&stream_)) {
            return detail::encode_zlib_content(*deflater, input, options.max_encoded_bytes_);
        }
        return detail::encode_zstd_content(
            std::get<detail::zstd_encoder_stream>(stream_), input, options.max_encoded_bytes_);
    }

    stream_type stream_;

private:
    [[nodiscard]] static stream_type make_stream(http_content_coding coding) {
        if (coding == http_content_coding::zstd) {
            return stream_type(std::in_place_type<detail::zstd_encoder_stream>);
        }
        return stream_type(std::in_place_type<detail::zlib_deflate_stream>,
            coding == http_content_coding::gzip ? detail::zlib_content_format::gzip
                                                : detail::zlib_content_format::deflate);
    }
};

http_content_encode_context::http_content_encode_context(
    http_content_coding coding) noexcept
    : coding_(coding) {}

http_content_encode_context::~http_content_encode_context() {
    delete impl_;
}

std::optional<std::string> http_content_encode_context::encode(
    std::string_view input, http_content_encode_options options) {
    switch (coding_) {
        case http_content_coding::gzip:
        case http_content_coding::deflate:
        case http_content_coding::zstd:
            break;
        case http_content_coding::brotli: {
            detail::brotli_encoder_stream encoder(detail::brotli_whole_buffer_window_bits);
            if (!encoder.initialized()) {
                throw http_content_codec_error("brotli encoder initialization failed");
            }
            return detail::encode_brotli_content(encoder, input, options.max_encoded_bytes_);
        }
        default:
            // identity and unknown values keep the one-shot facade's contract.
            return encode_http_content(coding_, input, options);
    }

    if (detail::acquire_codec(impl_, coding_) == nullptr) {
        throw http_content_codec_error("content encoder initialization failed");
    }
    const detail::codec_reset_guard reset_guard(impl_);
    return impl_->encode(input, options);
}

namespace {

class streaming_decode_stage final {
public:
    streaming_decode_stage(http_content_coding coding,
        std::size_t max_decoded_bytes, std::size_t max_memory_bytes)
        : coding_(coding),
          max_decoded_bytes_(max_decoded_bytes) {
        emplace_decoder_stream(coding, max_decoded_bytes, max_memory_bytes);
    }
    streaming_decode_stage(const streaming_decode_stage&) = delete;
    streaming_decode_stage& operator=(const streaming_decode_stage&) = delete;

    void write(std::string_view input, std::string& output) {
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
        std::size_t max_decoded_bytes, std::size_t max_memory_bytes) {
        switch (coding) {
            case http_content_coding::gzip:
            case http_content_coding::deflate: {
                auto& var = stream_.emplace(std::in_place_type<detail::zlib_inflate_stream>,
                    coding == http_content_coding::gzip ? detail::zlib_gzip_window_bits
                                                        : detail::zlib_deflate_window_bits,
                    max_memory_bytes);
                if (!std::get<detail::zlib_inflate_stream>(var).initialized()) {
                    throw http_content_codec_error("failed to initialize zlib content decoder");
                }
                return;
            }
            case http_content_coding::brotli: {
                auto& var = stream_.emplace(std::in_place_type<detail::brotli_decoder_stream>,
                    max_decoded_bytes, max_memory_bytes);
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
                    stream_.emplace(std::in_place_type<detail::zstd_decoder_stream>, max_memory_bytes);
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

    void append_checked(std::string& output, const char* data, std::size_t size) {
        if (size > max_decoded_bytes_ - decoded_bytes_) {
            throw http_content_transcode_error(
                http_content_decode_error::decoded_size_exceeded,
                "decoded HTTP content size exceeded limit");
        }
        output.append(data, size);
        decoded_bytes_ += size;
    }

    void write_zlib(
        detail::zlib_inflate_stream& inflater, std::string_view input, std::string& output) {
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
        detail::brotli_decoder_stream& decoder, std::string_view input, std::string& output) {
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
        detail::zstd_decoder_stream& decoder, std::string_view input, std::string& output) {
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
    using stage_ptr = std::unique_ptr<streaming_decode_stage>;

    impl_type(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_content_transcoder_options options)
        : options_(options),
          target_coding_(target_coding),
          encoder_(target_coding,
              http_content_encoder_options{
                  .max_encoded_bytes_ = options.max_encoded_bytes_,
                  .max_memory_bytes_ = options.max_memory_bytes_}) {
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
            stages_.push_back(std::make_unique<streaming_decode_stage>(
                coding, options_.max_decoded_bytes_, options_.max_memory_bytes_));
        }
        identity_decoded_bytes_ = 0;
    }

    void write(std::string_view input, std::string& output, bool flush) {
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
        std::string* current = &scratch_a_;
        std::string* next = &scratch_b_;
        for (std::size_t index = 1; index < stages_.size(); ++index) {
            next->clear();
            stages_[index]->write(*current, *next);
            std::swap(current, next);
        }
        if (!current->empty() || flush) {
            encoder_.write(*current, output, flush);
        }
    }

    void finish(std::string& output) {
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

    std::vector<http_content_coding> source_codings_;
    std::vector<stage_ptr> stages_;
    std::string scratch_a_;
    std::string scratch_b_;
    http_content_transcoder_options options_;
    std::size_t identity_decoded_bytes_{0};
    http_content_coding target_coding_;
    http_content_encoder encoder_;
};

http_content_transcoder::http_content_transcoder(http_content_coding source_coding,
    http_content_coding target_coding, http_content_transcoder_options options)
    : http_content_transcoder(
          std::span<const http_content_coding>(&source_coding, 1), target_coding, options) {}

http_content_transcoder::http_content_transcoder(
    std::span<const http_content_coding> source_codings, http_content_coding target_coding,
    http_content_transcoder_options options)
    : target_coding_(target_coding),
      impl_(new impl_type(source_codings, target_coding, options)) {}

http_content_transcoder::~http_content_transcoder() {
    delete impl_;
}

void http_content_transcoder::write(
    std::string_view input, std::string& output, bool flush) {
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

void http_content_transcoder::finish(std::string& output) {
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

struct http_staged_content_transcoder::impl_type final {
    impl_type(std::span<const http_content_coding> source_codings,
        http_content_coding target_coding, http_staged_transcode_options options)
        : min_bytes_(options.min_bytes_),
          max_bytes_(options.max_bytes_),
          max_memory_bytes_(options.max_memory_bytes_),
          target_coding_(target_coding),
          require_smaller_(options.require_smaller_than_source_) {
        if (min_bytes_ > max_bytes_) {
            throw std::invalid_argument(
                "staged transcoder minimum size must not exceed maximum size");
        }
        if (http_content_coding_token(target_coding_).empty()) {
            throw std::invalid_argument("unsupported HTTP target content coding");
        }
        for (const auto coding : source_codings) {
            if (http_content_coding_token(coding).empty()) {
                throw std::invalid_argument("unsupported HTTP source content coding");
            }
            if (coding != http_content_coding::identity) {
                source_codings_.push_back(coding);
            }
        }
        if (!source_codings_.empty() && max_bytes_ > 0) {
            const std::size_t decode_cap =
                max_bytes_ == (std::numeric_limits<std::size_t>::max)() ? max_bytes_ : max_bytes_ + 1U;
            decoder_.emplace(source_codings_, http_content_coding::identity,
                http_content_transcoder_options{
                    .max_decoded_bytes_ = decode_cap,
                    .max_memory_bytes_ = max_memory_bytes_,
                });
        }
    }

    void switch_to_passthrough() noexcept {
        passthrough_ = true;
        decoder_.reset();
        std::string().swap(staged_decoded_);
    }

    bool write(std::string_view input, std::string* passthrough_out) {
        if (passthrough_) {
            if (!input.empty()) {
                if (passthrough_out != nullptr) {
                    if (!staged_raw_.empty()) {
                        passthrough_out->append(staged_raw_);
                        std::string().swap(staged_raw_);
                    }
                    passthrough_out->append(input);
                } else {
                    staged_raw_.append(input);
                }
            } else if (passthrough_out != nullptr && !staged_raw_.empty()) {
                passthrough_out->append(staged_raw_);
                std::string().swap(staged_raw_);
            }
            return false;
        }

        if (input.size() > max_bytes_ || staged_raw_.size() > max_bytes_ - input.size()) {
            switch_to_passthrough();
            if (passthrough_out != nullptr) {
                if (!staged_raw_.empty()) {
                    passthrough_out->append(staged_raw_);
                    std::string().swap(staged_raw_);
                }
                passthrough_out->append(input);
            } else {
                staged_raw_.append(input);
            }
            return false;
        }

        if (input.size() > max_memory_bytes_ - staged_raw_.size()) {
            throw std::bad_alloc();
        }
        staged_raw_.append(input);
        if (source_codings_.empty()) {
            decoded_bytes_ = staged_raw_.size();
            return true;
        }

        try {
            decoder_->write(input, staged_decoded_, false);
            decoded_bytes_ = staged_decoded_.size();
            if (decoded_bytes_ > max_bytes_) {
                switch_to_passthrough();
                if (passthrough_out != nullptr && !staged_raw_.empty()) {
                    passthrough_out->append(staged_raw_);
                    std::string().swap(staged_raw_);
                }
                return false;
            }
        } catch (const http_content_transcode_error& err) {
            if (err.decode_error() == http_content_decode_error::decoded_size_exceeded) {
                switch_to_passthrough();
                if (passthrough_out != nullptr && !staged_raw_.empty()) {
                    passthrough_out->append(staged_raw_);
                    std::string().swap(staged_raw_);
                }
                return false;
            }
            throw;
        }
        return true;
    }

    http_staged_transcode_result finish() {
        const http_content_coding fallback_coding =
            source_codings_.empty() ? http_content_coding::identity : source_codings_.back();
        if (passthrough_) {
            return http_staged_transcode_result(
                false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
        }
        if (!source_codings_.empty()) {
            try {
                decoder_->finish(staged_decoded_);
                decoded_bytes_ = staged_decoded_.size();
                if (decoded_bytes_ > max_bytes_) {
                    switch_to_passthrough();
                    return http_staged_transcode_result(
                        false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
                }
            } catch (const http_content_transcode_error& err) {
                if (err.decode_error() == http_content_decode_error::decoded_size_exceeded) {
                    switch_to_passthrough();
                    return http_staged_transcode_result(
                        false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
                }
                throw;
            }
        } else {
            decoded_bytes_ = staged_raw_.size();
        }

        const std::string_view plain_view =
            source_codings_.empty() ? std::string_view(staged_raw_) : std::string_view(staged_decoded_);
        if (plain_view.size() < min_bytes_ || plain_view.size() > max_bytes_ ||
            (source_codings_.empty() && target_coding_ == http_content_coding::identity)) {
            return http_staged_transcode_result(
                false, fallback_coding, plain_view.size(), std::move(staged_raw_));
        }
        if (target_coding_ == http_content_coding::identity) {
            return http_staged_transcode_result(true, http_content_coding::identity,
                plain_view.size(), std::move(staged_decoded_));
        }

        const std::size_t max_encoded =
            require_smaller_
                ? (staged_raw_.empty() ? 0U : staged_raw_.size() - 1U)
                : (std::numeric_limits<std::size_t>::max)();
        std::optional<std::string> encoded;
        if (max_memory_bytes_ == (std::numeric_limits<std::size_t>::max)()) {
            encoded = encode_http_content(target_coding_, plain_view,
                http_content_encode_options{
                    .max_encoded_bytes_ = max_encoded,
                });
        } else {
            switch (target_coding_) {
                case http_content_coding::gzip:
                case http_content_coding::deflate: {
                    detail::zlib_deflate_stream deflater(
                        target_coding_ == http_content_coding::gzip
                            ? detail::zlib_content_format::gzip
                            : detail::zlib_content_format::deflate,
                        max_memory_bytes_);
                    if (!deflater.initialized()) {
                        throw http_content_codec_error("zlib encoder initialization failed");
                    }
                    encoded = detail::encode_zlib_content(deflater, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::brotli: {
                    detail::brotli_encoder_stream encoder(
                        detail::brotli_whole_buffer_window_bits, max_memory_bytes_);
                    if (!encoder.initialized()) {
                        throw http_content_codec_error("brotli encoder initialization failed");
                    }
                    encoded = detail::encode_brotli_content(encoder, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::zstd: {
                    detail::zstd_encoder_stream encoder(max_memory_bytes_);
                    if (!encoder.initialized()) {
                        throw http_content_codec_error("zstd encoder initialization failed");
                    }
                    encoded = detail::encode_zstd_content(encoder, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::identity:
                    break;
            }
        }
        if (!encoded.has_value()) {
            return http_staged_transcode_result(
                false, fallback_coding, plain_view.size(), std::move(staged_raw_));
        }
        return http_staged_transcode_result(
            true, target_coding_, plain_view.size(), std::move(*encoded));
    }

    void reset() {
        staged_raw_.clear();
        staged_decoded_.clear();
        decoded_bytes_ = 0;
        passthrough_ = false;
        if (!source_codings_.empty() && max_bytes_ > 0) {
            const std::size_t decode_cap =
                max_bytes_ == (std::numeric_limits<std::size_t>::max)() ? max_bytes_ : max_bytes_ + 1U;
            if (decoder_.has_value()) {
                decoder_->reset();
            } else {
                decoder_.emplace(source_codings_, http_content_coding::identity,
                    http_content_transcoder_options{
                        .max_decoded_bytes_ = decode_cap,
                        .max_memory_bytes_ = max_memory_bytes_,
                    });
            }
        } else {
            decoder_.reset();
        }
    }

    std::vector<http_content_coding> source_codings_;
    std::optional<http_content_transcoder> decoder_;
    std::string staged_raw_;
    std::string staged_decoded_;
    std::size_t min_bytes_;
    std::size_t max_bytes_;
    std::size_t max_memory_bytes_;
    std::size_t decoded_bytes_{0};
    http_content_coding target_coding_;
    bool require_smaller_{false};
    bool passthrough_{false};
};

http_staged_content_transcoder::http_staged_content_transcoder(
    http_content_coding source_coding, http_content_coding target_coding,
    http_staged_transcode_options options)
    : http_staged_content_transcoder(
          std::span<const http_content_coding>(&source_coding, 1), target_coding, options) {}

http_staged_content_transcoder::http_staged_content_transcoder(
    std::span<const http_content_coding> source_codings, http_content_coding target_coding,
    http_staged_transcode_options options)
    : target_coding_(target_coding),
      impl_(new impl_type(source_codings, target_coding, options)) {}

http_staged_content_transcoder::~http_staged_content_transcoder() {
    delete impl_;
}

bool http_staged_content_transcoder::passthrough() const noexcept {
    return impl_ != nullptr && impl_->passthrough_;
}

std::size_t http_staged_content_transcoder::staged_raw_bytes() const noexcept {
    return impl_ != nullptr ? impl_->staged_raw_.size() : 0;
}

std::size_t http_staged_content_transcoder::decoded_bytes() const noexcept {
    return impl_ != nullptr ? impl_->decoded_bytes_ : 0;
}

bool http_staged_content_transcoder::write(std::string_view input) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder is no longer writable");
    }
    try {
        return impl_->write(input, nullptr);
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

bool http_staged_content_transcoder::write(
    std::string_view input, std::string& passthrough_output) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder is no longer writable");
    }
    try {
        if (detail::http_text_aliases_string(input, passthrough_output)) {
            const std::string owned(input);
            return impl_->write(owned, &passthrough_output);
        }
        return impl_->write(input, &passthrough_output);
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

http_staged_transcode_result http_staged_content_transcoder::finish() {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder cannot finish in current state");
    }
    try {
        auto result = impl_->finish();
        phase_ = phase::finished;
        return result;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

void http_staged_content_transcoder::reset() {
    try {
        impl_->reset();
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
    phase_ = phase::active;
}

http_worker_codecs::http_worker_codecs() noexcept
    : gzip_(http_content_coding::gzip),
      deflate_(http_content_coding::deflate),
      brotli_(http_content_coding::brotli),
      zstd_(http_content_coding::zstd) {}

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
    return http_stream_encoder_lease(*this, *(new http_content_encoder(coding)));
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
    delete &encoder;
}
}  // namespace ruvia
