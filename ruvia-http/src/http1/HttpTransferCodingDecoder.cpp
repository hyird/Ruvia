#include "ruvia/http/HttpTransferCodingDecoder.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

#include "ruvia/http/detail/coding/ZlibPmrAllocation.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia {

HttpTransferCodingDecoder::HttpTransferCodingDecoder(
    HttpTransferCoding coding, std::pmr::memory_resource* resource, ProtocolByteLimit bodyLimit)
    : resource_(detail::httpPmrResourceOrDefault(resource)),
      bodyLimit_(bodyLimit),
      coding_(coding) {
    int windowBits = 0;
    switch (coding) {
        case HttpTransferCoding::kGzip:
            windowBits = 15 + 16;
            break;
        case HttpTransferCoding::kDeflate:
            windowBits = 15;
            break;
        default:
            throw std::invalid_argument("unsupported HTTP transfer coding");
    }
    stream_.zalloc = &HttpTransferCodingDecoder::zallocThunk;
    stream_.zfree = &HttpTransferCodingDecoder::zfreeThunk;
    stream_.opaque = this;
    const int rc = inflateInit2(&stream_, windowBits);
    if (rc == Z_MEM_ERROR) {
        throw std::bad_alloc();
    }
    if (rc != Z_OK) {
        throw std::runtime_error("failed to initialize transfer-coding decoder");
    }
}

HttpTransferCodingDecoder::~HttpTransferCodingDecoder() {
    (void)inflateEnd(&stream_);
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::decode(
    std::string_view input, std::span<char> output_buffer) noexcept {
    if (std::holds_alternative<DecoderFailed>(state_)) {
        return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(0));
    }
    if (const auto* failure = std::get_if<HttpTransferCodingDecodeError>(&state_)) {
        return fail(0, *failure);
    }
    if (std::holds_alternative<Complete>(state_)) {
        return input.empty() ? complete(0) : fail(0, HttpTransferCodingDecodeError::kInvalidContent);
    }
    if (output_buffer.empty()) {
        return failDecoder(0);
    }

    std::size_t consumed = 0;
    std::size_t produced = 0;
    for (;;) {
        if (std::holds_alternative<GzipMemberBoundary>(state_)) {
            if (consumed == input.size()) {
                return produced != 0
                           ? output(consumed, std::string_view(output_buffer.data(), produced))
                           : needInput(consumed);
            }
            if (inflateReset(&stream_) != Z_OK) {
                return failDecoder(consumed);
            }
            state_.emplace<Active>();
        }

        const auto step = inflateStep(input.substr(consumed), output_buffer.subspan(produced));
        consumed += step.consumed;
        if (bodyLimit_.additionExceeds(decodedBytes_, step.produced)) {
            return fail(consumed, HttpTransferCodingDecodeError::kDecodedSizeExceeded);
        }
        decodedBytes_ += step.produced;
        produced += step.produced;

        if (step.status == Z_STREAM_END) {
            if (coding_ != HttpTransferCoding::kGzip) {
                if (consumed != input.size()) {
                    return fail(consumed, HttpTransferCodingDecodeError::kInvalidContent);
                }
                state_.emplace<Complete>();
                return produced != 0
                           ? output(consumed, std::string_view(output_buffer.data(), produced))
                           : complete(consumed);
            }

            // RFC 1952 gzip data is a series of members. A member boundary is
            // not the end of the transfer coding: another member can arrive in
            // the same HTTP chunk or in a later one. Only framing EOF, reported
            // through finishInput(), commits this boundary as complete.
            state_.emplace<GzipMemberBoundary>();
            if (produced == output_buffer.size()) {
                return output(consumed, std::string_view(output_buffer.data(), produced));
            }
            continue;
        }

        if (step.status != Z_OK && step.status != Z_BUF_ERROR) {
            if (step.status == Z_DATA_ERROR || step.status == Z_NEED_DICT) {
                return fail(consumed, HttpTransferCodingDecodeError::kInvalidContent);
            }
            return failDecoder(consumed);
        }

        if (produced != 0) {
            return output(consumed, std::string_view(output_buffer.data(), produced));
        }
        if (consumed != input.size()) {
            return fail(consumed, HttpTransferCodingDecodeError::kInvalidContent);
        }
        return needInput(consumed);
    }
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::finishInput() noexcept {
    if (std::holds_alternative<DecoderFailed>(state_)) {
        return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(0));
    }
    if (std::holds_alternative<Complete>(state_) ||
        std::holds_alternative<GzipMemberBoundary>(state_)) {
        state_.emplace<Complete>();
        return complete(0);
    }
    if (const auto* failure = std::get_if<HttpTransferCodingDecodeError>(&state_)) {
        return fail(0, *failure);
    }
    return fail(0, HttpTransferCodingDecodeError::kInvalidContent);
}

HttpTransferCodingDecoder::InflateStep HttpTransferCodingDecoder::inflateStep(
    std::string_view input, std::span<char> output) noexcept {
    const auto inputBytes = std::min<std::size_t>(input.size(), (std::numeric_limits<uInt>::max)());
    stream_.next_in =
        inputBytes == 0 ? Z_NULL : reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream_.avail_in = static_cast<uInt>(inputBytes);
    const auto outputBytes =
        std::min<std::size_t>(output.size(), (std::numeric_limits<uInt>::max)());
    stream_.next_out = reinterpret_cast<Bytef*>(output.data());
    stream_.avail_out = static_cast<uInt>(outputBytes);

    const auto status = inflate(&stream_, Z_NO_FLUSH);
    const auto consumed = inputBytes - stream_.avail_in;
    const auto produced = outputBytes - stream_.avail_out;
    stream_.next_in = Z_NULL;
    stream_.avail_in = 0;
    stream_.next_out = Z_NULL;
    stream_.avail_out = 0;
    return InflateStep{consumed, produced, status};
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::needInput(std::size_t consumed) noexcept {
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeNeedInput(consumed));
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::output(
    std::size_t consumed, std::string_view bytes) noexcept {
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeOutputView(consumed, bytes));
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::complete(std::size_t consumed) noexcept {
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeComplete(consumed));
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::fail(
    std::size_t consumed, HttpTransferCodingDecodeError error) noexcept {
    state_.emplace<HttpTransferCodingDecodeError>(error);
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeFailure(consumed, error));
}

HttpTransferCodingDecodeResult HttpTransferCodingDecoder::failDecoder(std::size_t consumed) noexcept {
    state_.emplace<DecoderFailed>();
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(consumed));
}

voidpf HttpTransferCodingDecoder::zallocThunk(voidpf opaque, uInt items, uInt size) noexcept {
    auto* self = static_cast<HttpTransferCodingDecoder*>(opaque);
    return self == nullptr ? nullptr : detail::zlibPmrAllocate(self->resource_, items, size);
}

void HttpTransferCodingDecoder::zfreeThunk(voidpf, voidpf address) noexcept {
    detail::zlibPmrFree(address);
}

namespace {

constexpr std::size_t k_transfer_coding_scratch_bytes = 4096;

struct transfer_decoder_deleter final {
    std::pmr::memory_resource* resource;
    void operator()(HttpTransferCodingDecoder* decoder) const noexcept {
        if (decoder != nullptr) {
            std::pmr::polymorphic_allocator<HttpTransferCodingDecoder>(resource)
                .delete_object(decoder);
        }
    }
};

struct transfer_coding_stage final {
    using decoder_ptr = std::unique_ptr<HttpTransferCodingDecoder, transfer_decoder_deleter>;

    explicit transfer_coding_stage(decoder_ptr decoder)
        : decoder(std::move(decoder)) {}
    transfer_coding_stage(transfer_coding_stage&&) noexcept = default;
    transfer_coding_stage& operator=(transfer_coding_stage&&) noexcept = default;
    transfer_coding_stage(const transfer_coding_stage&) = delete;
    transfer_coding_stage& operator=(const transfer_coding_stage&) = delete;

    decoder_ptr decoder;
    std::array<char, k_transfer_coding_scratch_bytes> buffer{};
    std::size_t begin{0};
    std::size_t end{0};
    bool complete{false};
};

}  // namespace

struct http_transfer_coding_stack_decoder::impl final {
    impl(std::span<const HttpTransferCoding> codings, std::pmr::memory_resource* resource,
        ProtocolByteLimit decoded_limit)
        : resource(detail::httpPmrResourceOrDefault(resource)),
          stages(this->resource) {
        if (codings.empty() || codings.size() > kMaxTransferCodings) {
            throw std::invalid_argument("invalid transfer-coding stack length");
        }
        stages.reserve(codings.size());
        std::pmr::polymorphic_allocator<HttpTransferCodingDecoder> allocator(this->resource);
        for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
            auto* decoder = allocator.new_object<HttpTransferCodingDecoder>(
                *coding, this->resource, decoded_limit);
            stages.emplace_back(transfer_coding_stage::decoder_ptr(
                decoder, transfer_decoder_deleter{this->resource}));
        }
    }

    std::pmr::memory_resource* resource;
    std::pmr::vector<transfer_coding_stage> stages;
    std::optional<HttpTransferCodingDecodeError> failure;
    bool decoder_failed{false};
};

http_transfer_coding_stack_decoder::http_transfer_coding_stack_decoder(
    std::span<const HttpTransferCoding> codings, std::pmr::memory_resource* resource,
    ProtocolByteLimit decoded_limit) {
    auto* memory = detail::httpPmrResourceOrDefault(resource);
    std::pmr::polymorphic_allocator<impl> allocator(memory);
    impl_ = allocator.new_object<impl>(codings, memory, decoded_limit);
}

http_transfer_coding_stack_decoder::~http_transfer_coding_stack_decoder() {
    if (impl_ != nullptr) {
        std::pmr::polymorphic_allocator<impl>(impl_->resource).delete_object(impl_);
    }
}

HttpTransferCodingDecodeResult http_transfer_coding_stack_decoder::decode(
    std::string_view input, std::span<char> output_buffer) noexcept {
    const auto failed = [this](std::size_t consumed) {
        if (impl_->decoder_failed) {
            return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(consumed));
        }
        return HttpTransferCodingDecodeResult(
            HttpTransferCodingDecodeFailure(consumed, *impl_->failure));
    };
    if (impl_->decoder_failed || impl_->failure.has_value()) {
        return failed(0);
    }
    if (output_buffer.empty()) {
        impl_->decoder_failed = true;
        return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(0));
    }

    std::size_t wire_consumed = 0;
    for (;;) {
        bool progressed = false;
        for (std::size_t index = 0; index < impl_->stages.size(); ++index) {
            auto& stage = impl_->stages[index];
            if (index + 1 < impl_->stages.size() && stage.begin != stage.end) {
                continue;
            }
            std::string_view layer_input;
            if (index == 0) {
                layer_input = input.substr(wire_consumed);
            } else {
                auto& previous = impl_->stages[index - 1];
                layer_input = std::string_view(previous.buffer.data() + previous.begin,
                    previous.end - previous.begin);
            }
            if (stage.complete) {
                if (!layer_input.empty()) {
                    impl_->failure = HttpTransferCodingDecodeError::kInvalidContent;
                    return failed(wire_consumed);
                }
                continue;
            }
            const auto output = index + 1 == impl_->stages.size()
                                    ? output_buffer
                                    : std::span<char>(stage.buffer);
            const auto decoded = stage.decoder->decode(layer_input, output);
            const auto consumed = decoded.consumedBytes();
            if (index == 0) {
                wire_consumed += consumed;
            } else {
                auto& previous = impl_->stages[index - 1];
                previous.begin += consumed;
                if (previous.begin == previous.end) {
                    previous.begin = 0;
                    previous.end = 0;
                }
            }
            progressed = progressed || consumed != 0;
            if (const auto* bytes = decoded.output()) {
                progressed = true;
                if (index + 1 == impl_->stages.size()) {
                    return HttpTransferCodingDecodeResult(
                        HttpTransferCodingDecodeOutputView(wire_consumed, bytes->bytes()));
                }
                stage.begin = 0;
                stage.end = bytes->bytes().size();
                continue;
            }
            if (const auto* protocol_failure = decoded.failure()) {
                impl_->failure = protocol_failure->error();
                return failed(wire_consumed);
            }
            if (decoded.decoderFailure() != nullptr) {
                impl_->decoder_failed = true;
                return failed(wire_consumed);
            }
            if (decoded.complete() != nullptr) {
                stage.complete = true;
                progressed = true;
            } else if (decoded.needInput() == nullptr) {
                impl_->decoder_failed = true;
                return failed(wire_consumed);
            }
        }

        bool all_complete = true;
        bool pending = false;
        for (std::size_t index = 0; index < impl_->stages.size(); ++index) {
            const auto& stage = impl_->stages[index];
            all_complete = all_complete && stage.complete;
            pending = pending || (index + 1 < impl_->stages.size() && stage.begin != stage.end);
        }
        if (all_complete && !pending) {
            return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeComplete(wire_consumed));
        }
        if (!progressed) {
            return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeNeedInput(wire_consumed));
        }
    }
}

HttpTransferCodingDecodeResult http_transfer_coding_stack_decoder::finish_input() noexcept {
    if (impl_->decoder_failed) {
        return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(0));
    }
    if (impl_->failure.has_value()) {
        return HttpTransferCodingDecodeResult(
            HttpTransferCodingDecodeFailure(0, *impl_->failure));
    }
    for (std::size_t index = 0; index < impl_->stages.size(); ++index) {
        auto& stage = impl_->stages[index];
        if (index + 1 < impl_->stages.size() && stage.begin != stage.end) {
            return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeNeedInput(0));
        }
        if (stage.complete) {
            continue;
        }
        const auto finished = stage.decoder->finishInput();
        if (const auto* protocol_failure = finished.failure()) {
            impl_->failure = protocol_failure->error();
            return HttpTransferCodingDecodeResult(
                HttpTransferCodingDecodeFailure(0, *impl_->failure));
        }
        if (finished.decoderFailure() != nullptr) {
            impl_->decoder_failed = true;
            return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(0));
        }
        if (finished.complete() == nullptr) {
            return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeNeedInput(0));
        }
        stage.complete = true;
    }
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeComplete(0));
}

}  // namespace ruvia
