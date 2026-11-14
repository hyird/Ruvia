#include "http_content_transcoder.h"

#include <brotli/decode.h>
#include <zlib.h>
#include <zstd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "http_compression_codec.h"
#include "http_content_codec.h"
#include "http_content_coding.h"
#include "http_field_text.h"

namespace ruvia {

namespace {

class streaming_decode_stage final {
public:
    streaming_decode_stage(http_content_coding coding, std::size_t max_decoded_bytes, std::size_t max_memory_bytes) :
        coding_(coding),
        max_decoded_bytes_(max_decoded_bytes) {
        emplace_decoder_stream(coding, max_decoded_bytes, max_memory_bytes);
    }
    streaming_decode_stage(const streaming_decode_stage&) = delete;
    streaming_decode_stage& operator=(const streaming_decode_stage&) = delete;

    [[nodiscard]] std::optional<http_content_decode_error> write(std::string_view input, std::string& output) {
        if (input.empty()) {
            return std::nullopt;
        }
        saw_input_ = true;
        if (auto* zlib_decoder = std::get_if<detail::zlib_inflate_stream>(&*stream_)) {
            return write_zlib(*zlib_decoder, input, output);
        }
        if (auto* brotli_decoder = std::get_if<detail::brotli_decoder_stream>(&*stream_)) {
            return write_brotli(*brotli_decoder, input, output);
        }
        return write_zstd(std::get<detail::zstd_decoder_stream>(*stream_), input, output);
    }

    [[nodiscard]] std::optional<http_content_decode_error> finish() const noexcept {
        if (saw_input_ && !at_frame_end_) {
            return http_content_decode_error::invalid_content;
        }
        return std::nullopt;
    }

private:
    using decoder_variant = std::variant<detail::zlib_inflate_stream, detail::brotli_decoder_stream, detail::zstd_decoder_stream>;

    void emplace_decoder_stream(http_content_coding coding, std::size_t max_decoded_bytes, std::size_t max_memory_bytes) {
        switch (coding) {
            case http_content_coding::gzip:
            case http_content_coding::deflate: {
                auto& var = stream_.emplace(std::in_place_type<detail::zlib_inflate_stream>, coding == http_content_coding::gzip ? detail::zlib_gzip_window_bits : detail::zlib_deflate_window_bits, max_memory_bytes);
                if (!std::get<detail::zlib_inflate_stream>(var).initialized()) {
                    throw http_content_codec_error("failed to initialize zlib content decoder");
                }
                return;
            }
            case http_content_coding::brotli: {
                // The decoded-size budget always admits the initial state, so a
                // refusal here is a codec failure; the memory cap already threw.
                auto& var = stream_.emplace(std::in_place_type<detail::brotli_decoder_stream>, max_decoded_bytes, max_memory_bytes);
                if (!std::get<detail::brotli_decoder_stream>(var).initialized()) {
                    throw http_content_codec_error("failed to initialize Brotli content decoder");
                }
                return;
            }
            case http_content_coding::zstd: {
                auto& var = stream_.emplace(std::in_place_type<detail::zstd_decoder_stream>, max_memory_bytes);
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

    [[nodiscard]] bool append_checked(std::string& output, const char* data, std::size_t size) {
        if (size > max_decoded_bytes_ - decoded_bytes_) {
            return false;
        }
        output.append(data, size);
        decoded_bytes_ += size;
        return true;
    }

    [[nodiscard]] std::optional<http_content_decode_error> write_zlib(detail::zlib_inflate_stream& inflater, std::string_view input, std::string& output) {
        const bool allow_concatenated = coding_ == http_content_coding::gzip;
        const int window_bits = allow_concatenated ? detail::zlib_gzip_window_bits : detail::zlib_deflate_window_bits;
        auto& stream = inflater.native();
        const detail::zlib_borrow_scope borrow(stream);
        if (at_frame_end_) {
            if (!allow_concatenated) {
                return http_content_decode_error::invalid_content;
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
            if (produced != 0 && !append_checked(output, buffer.data(), produced)) {
                return http_content_decode_error::decoded_size_exceeded;
            }
            if (status == Z_STREAM_END) {
                at_frame_end_ = true;
                detail::zlib_refill_input(stream, input, supplied);
                if (stream.avail_in == 0 && supplied == input.size()) {
                    return std::nullopt;
                }
                if (!allow_concatenated) {
                    return http_content_decode_error::invalid_content;
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
                return http_content_decode_error::invalid_content;
            }
            const bool progressed = produced != 0 || stream.avail_in != before_input;
            if (!progressed) {
                if (stream.avail_in == 0 && supplied < input.size()) {
                    continue;
                }
                if (stream.avail_in == 0 && supplied == input.size()) {
                    return std::nullopt;
                }
                return http_content_decode_error::invalid_content;
            }
        }
    }

    [[nodiscard]] std::optional<http_content_decode_error> write_brotli(detail::brotli_decoder_stream& decoder, std::string_view input, std::string& output) {
        if (at_frame_end_) {
            return http_content_decode_error::invalid_content;
        }
        auto* const state_value = decoder.native();
        const auto* next_input = reinterpret_cast<const std::uint8_t*>(input.data());
        std::size_t available_input = input.size();
        for (;;) {
            const auto before_input = available_input;
            std::size_t available_output = 0;
            const auto result_value = BrotliDecoderDecompressStream(state_value, &available_input, &next_input, &available_output, nullptr, nullptr);
            bool produced_output = false;
            while (BrotliDecoderHasMoreOutput(state_value) == BROTLI_TRUE) {
                std::size_t produced = 0;
                const auto* const bytes_value = BrotliDecoderTakeOutput(state_value, &produced);
                if (produced != 0) {
                    if (!append_checked(output, reinterpret_cast<const char*>(bytes_value), produced)) {
                        return http_content_decode_error::decoded_size_exceeded;
                    }
                    produced_output = true;
                }
            }
            if (result_value == BROTLI_DECODER_RESULT_SUCCESS) {
                if (available_input != 0) {
                    return http_content_decode_error::invalid_content;
                }
                at_frame_end_ = true;
                return std::nullopt;
            }
            if (result_value == BROTLI_DECODER_RESULT_ERROR) {
                // A ring buffer refused by the native budget was declared larger
                // than the decoded-size cap allows the content to be.
                if (decoder.native_budget_exceeded()) {
                    return http_content_decode_error::decoded_size_exceeded;
                }
                decoder.rethrow_allocation_failure();
                return http_content_decode_error::invalid_content;
            }
            if (result_value == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && available_input == 0) {
                return std::nullopt;
            }
            if (!produced_output && available_input == before_input) {
                return http_content_decode_error::invalid_content;
            }
        }
    }

    [[nodiscard]] std::optional<http_content_decode_error> write_zstd(detail::zstd_decoder_stream& decoder, std::string_view input, std::string& output) {
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
                return http_content_decode_error::invalid_content;
            }
            if (out.pos != 0 && !append_checked(output, buffer.data(), out.pos)) {
                return http_content_decode_error::decoded_size_exceeded;
            }
            if (result_value == 0) {
                at_frame_end_ = true;
                if (in.pos == in.size) {
                    return std::nullopt;
                }
                at_frame_end_ = false;
                continue;
            }
            if (in.pos == in.size && out.pos < buffer.size()) {
                return std::nullopt;
            }
            if (out.pos == 0 && in.pos == before_input) {
                return http_content_decode_error::invalid_content;
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

    impl_type(std::span<const http_content_coding> source_codings, http_content_coding target_coding, http_content_transcoder_options options) :
        options_(options),
        target_coding_(target_coding),
        encoder_(target_coding, http_content_encoder_options{.max_encoded_bytes_ = options.max_encoded_bytes_, .max_memory_bytes_ = options.max_memory_bytes_}) {
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
            stages_.push_back(std::make_unique<streaming_decode_stage>(coding, options_.max_decoded_bytes_, options_.max_memory_bytes_));
        }
        identity_decoded_bytes_ = 0;
    }

    [[nodiscard]] std::optional<http_content_decode_error> write(std::string_view input, std::string& output, bool flush) {
        if (stages_.empty()) {
            if (input.size() > options_.max_decoded_bytes_ - identity_decoded_bytes_) {
                return http_content_decode_error::decoded_size_exceeded;
            }
            identity_decoded_bytes_ += input.size();
            encoder_.write(input, output, flush);
            return std::nullopt;
        }
        if (stages_.size() == 1 && target_coding_ == http_content_coding::identity && options_.max_encoded_bytes_ == (std::numeric_limits<std::size_t>::max)() && !detail::http_text_aliases_string(input, output)) {
            return stages_.front()->write(input, output);
        }
        scratch_a_.clear();
        if (const auto error = stages_.front()->write(input, scratch_a_)) {
            return error;
        }
        std::string* current = &scratch_a_;
        std::string* next = &scratch_b_;
        for (std::size_t index = 1; index < stages_.size(); ++index) {
            next->clear();
            if (const auto error = stages_[index]->write(*current, *next)) {
                return error;
            }
            std::swap(current, next);
        }
        if (!current->empty() || flush) {
            encoder_.write(*current, output, flush);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<http_content_decode_error> finish(std::string& output) {
        for (const auto& stage : stages_) {
            if (const auto error = stage->finish()) {
                return error;
            }
        }
        encoder_.finish(output);
        return std::nullopt;
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

http_content_transcoder::http_content_transcoder(http_content_coding source_coding, http_content_coding target_coding, http_content_transcoder_options options) :
    http_content_transcoder(std::span<const http_content_coding>(&source_coding, 1), target_coding, options) {}

http_content_transcoder::http_content_transcoder(std::span<const http_content_coding> source_codings, http_content_coding target_coding, http_content_transcoder_options options) :
    target_coding_(target_coding),
    impl_(new impl_type(source_codings, target_coding, options)) {}

http_content_transcoder::~http_content_transcoder() {
    delete impl_;
}

std::optional<http_content_decode_error> http_content_transcoder::write(std::string_view input, std::string& output, bool flush) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP content transcoder is no longer writable");
    }
    try {
        const auto error = impl_->write(input, output, flush);
        if (error.has_value()) {
            phase_ = phase::failed;
        }
        return error;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

std::optional<http_content_decode_error> http_content_transcoder::finish(std::string& output) {
    if (phase_ == phase::finished) {
        return std::nullopt;
    }
    if (phase_ == phase::failed) {
        throw std::logic_error("HTTP content transcoder has failed");
    }
    try {
        const auto error = impl_->finish(output);
        phase_ = error.has_value() ? phase::failed : phase::finished;
        return error;
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
    impl_type(std::span<const http_content_coding> source_codings, http_content_coding target_coding, http_staged_transcode_options options) :
        min_bytes_(options.min_bytes_),
        max_bytes_(options.max_bytes_),
        max_memory_bytes_(options.max_memory_bytes_),
        target_coding_(target_coding),
        require_smaller_(options.require_smaller_than_source_) {
        if (min_bytes_ > max_bytes_) {
            throw std::invalid_argument("staged transcoder minimum size must not exceed maximum size");
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
            const std::size_t decode_cap = max_bytes_ == (std::numeric_limits<std::size_t>::max)() ? max_bytes_ : max_bytes_ + 1U;
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

    [[nodiscard]] std::optional<http_content_decode_error> write(std::string_view input, std::string* passthrough_out) {
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
            return std::nullopt;
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
            return std::nullopt;
        }

        if (input.size() > max_memory_bytes_ - staged_raw_.size()) {
            throw std::bad_alloc();
        }
        staged_raw_.append(input);
        if (source_codings_.empty()) {
            decoded_bytes_ = staged_raw_.size();
            return std::nullopt;
        }
        if (input.empty()) {
            return std::nullopt;
        }

        const auto error = decoder_->write(input, staged_decoded_, false);
        if (error == http_content_decode_error::invalid_content) {
            return error;
        }
        // The decoder cap is max_bytes_ + 1, so either signal means the decoded
        // length has left the staging window.
        if (!error.has_value()) {
            decoded_bytes_ = staged_decoded_.size();
        }
        if (error.has_value() || decoded_bytes_ > max_bytes_) {
            switch_to_passthrough();
            if (passthrough_out != nullptr && !staged_raw_.empty()) {
                passthrough_out->append(staged_raw_);
                std::string().swap(staged_raw_);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::variant<http_staged_transcode_result, http_content_decode_error> finish() {
        const http_content_coding fallback_coding = source_codings_.empty() ? http_content_coding::identity : source_codings_.back();
        if (passthrough_) {
            return http_staged_transcode_result(false, fallback_coding, decoded_bytes_, std::move(staged_raw_));
        }
        if (source_codings_.empty()) {
            decoded_bytes_ = staged_raw_.size();
        } else if (decoder_.has_value()) {
            // Ending the stream only reports truncation; it appends no content.
            if (const auto error = decoder_->finish(staged_decoded_)) {
                return *error;
            }
        }

        const std::string_view plain_view = source_codings_.empty() ? std::string_view(staged_raw_) : std::string_view(staged_decoded_);
        if (plain_view.size() < min_bytes_ || plain_view.size() > max_bytes_ || (source_codings_.empty() && target_coding_ == http_content_coding::identity)) {
            return http_staged_transcode_result(false, fallback_coding, plain_view.size(), std::move(staged_raw_));
        }
        if (target_coding_ == http_content_coding::identity) {
            return http_staged_transcode_result(true, http_content_coding::identity, plain_view.size(), std::move(staged_decoded_));
        }

        const std::size_t max_encoded = require_smaller_ ? (staged_raw_.empty() ? 0U : staged_raw_.size() - 1U) : (std::numeric_limits<std::size_t>::max)();
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
                    detail::zlib_deflate_stream deflater(target_coding_ == http_content_coding::gzip ? detail::zlib_content_format::gzip : detail::zlib_content_format::deflate, max_memory_bytes_);
                    if (!deflater.initialized()) {
                        throw http_content_codec_error("zlib encoder initialization failed");
                    }
                    encoded = detail::encode_zlib_content(deflater, plain_view, max_encoded);
                    break;
                }
                case http_content_coding::brotli: {
                    detail::brotli_encoder_stream encoder(detail::brotli_whole_buffer_window_bits, max_memory_bytes_);
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
            return http_staged_transcode_result(false, fallback_coding, plain_view.size(), std::move(staged_raw_));
        }
        return http_staged_transcode_result(true, target_coding_, plain_view.size(), std::move(*encoded));
    }

    void reset() {
        staged_raw_.clear();
        staged_decoded_.clear();
        decoded_bytes_ = 0;
        passthrough_ = false;
        if (!source_codings_.empty() && max_bytes_ > 0) {
            const std::size_t decode_cap = max_bytes_ == (std::numeric_limits<std::size_t>::max)() ? max_bytes_ : max_bytes_ + 1U;
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

http_staged_content_transcoder::http_staged_content_transcoder(http_content_coding source_coding, http_content_coding target_coding, http_staged_transcode_options options) :
    http_staged_content_transcoder(std::span<const http_content_coding>(&source_coding, 1), target_coding, options) {}

http_staged_content_transcoder::http_staged_content_transcoder(std::span<const http_content_coding> source_codings, http_content_coding target_coding, http_staged_transcode_options options) :
    target_coding_(target_coding),
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

std::optional<http_content_decode_error> http_staged_content_transcoder::write(std::string_view input) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder is no longer writable");
    }
    try {
        const auto error = impl_->write(input, nullptr);
        if (error.has_value()) {
            phase_ = phase::failed;
        }
        return error;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

std::optional<http_content_decode_error> http_staged_content_transcoder::write(std::string_view input, std::string& passthrough_output) {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder is no longer writable");
    }
    try {
        std::optional<http_content_decode_error> error;
        if (detail::http_text_aliases_string(input, passthrough_output)) {
            const std::string owned(input);
            error = impl_->write(owned, &passthrough_output);
        } else {
            error = impl_->write(input, &passthrough_output);
        }
        if (error.has_value()) {
            phase_ = phase::failed;
        }
        return error;
    } catch (...) {
        phase_ = phase::failed;
        throw;
    }
}

std::variant<http_staged_transcode_result, http_content_decode_error> http_staged_content_transcoder::finish() {
    if (phase_ != phase::active) {
        throw std::logic_error("HTTP staged content transcoder cannot finish in current state");
    }
    try {
        auto result = impl_->finish();
        phase_ = std::holds_alternative<http_staged_transcode_result>(result) ? phase::finished : phase::failed;
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

}  // namespace ruvia
