#include "ruvia/http/HttpTransferCodingDecoder.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>

#include "ruvia/http/detail/util/PmrResource.h"

#include "coding/ZlibPmrAllocation.h"

namespace ruvia {

namespace detail {

// One zlib stage is an implementation detail of the coding sequence, not a
// second public decoder. Its address must stay fixed for zlib's opaque pointer.
class transfer_coding_decoder final {
public:
    transfer_coding_decoder(HttpTransferCoding coding, std::pmr::memory_resource* resource, ProtocolByteLimit decoded_limit);
    ~transfer_coding_decoder();
    transfer_coding_decoder(const transfer_coding_decoder&) = delete;
    transfer_coding_decoder& operator=(const transfer_coding_decoder&) = delete;
    [[nodiscard]] HttpTransferCodingDecodeResult decode(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] HttpTransferCodingDecodeResult finish_input() noexcept;

private:
    struct inflate_step_result {
        std::size_t consumed{0};
        std::size_t produced{0};
        int status{Z_OK};
    };
    struct active final {};
    struct gzip_member_boundary final {};
    struct completed final {};
    struct decoder_failed final {};
    using state = std::variant<active, gzip_member_boundary, completed, HttpTransferCodingDecodeError, decoder_failed>;
    [[nodiscard]] inflate_step_result inflate_step(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] static HttpTransferCodingDecodeResult need_input(std::size_t consumed) noexcept;
    [[nodiscard]] static HttpTransferCodingDecodeResult output(std::size_t consumed, std::string_view bytes) noexcept;
    [[nodiscard]] static HttpTransferCodingDecodeResult complete(std::size_t consumed) noexcept;
    [[nodiscard]] HttpTransferCodingDecodeResult fail(std::size_t consumed, HttpTransferCodingDecodeError error) noexcept;
    [[nodiscard]] HttpTransferCodingDecodeResult fail_decoder(std::size_t consumed) noexcept;
    static voidpf zalloc_thunk(voidpf opaque, uInt items, uInt size) noexcept;
    static void zfree_thunk(voidpf opaque, voidpf address) noexcept;
    z_stream stream_{};
    state state_{active{}};
    std::pmr::memory_resource* resource_;
    ProtocolByteLimit body_limit_;
    std::size_t decoded_bytes_{0};
    HttpTransferCoding coding_;
};

transfer_coding_decoder::transfer_coding_decoder(
    HttpTransferCoding coding, std::pmr::memory_resource* resource, ProtocolByteLimit decoded_limit)
    : resource_(detail::httpPmrResourceOrDefault(resource)),
      body_limit_(decoded_limit),
      coding_(coding) {
    int window_bits = 0;
    switch (coding) {
        case HttpTransferCoding::kGzip:
            window_bits = 15 + 16;
            break;
        case HttpTransferCoding::kDeflate:
            window_bits = 15;
            break;
        default:
            throw std::invalid_argument("unsupported HTTP transfer coding");
    }
    stream_.zalloc = &transfer_coding_decoder::zalloc_thunk;
    stream_.zfree = &transfer_coding_decoder::zfree_thunk;
    stream_.opaque = this;
    const int rc = inflateInit2(&stream_, window_bits);
    if (rc == Z_MEM_ERROR) {
        throw std::bad_alloc();
    }
    if (rc != Z_OK) {
        throw std::runtime_error("failed to initialize transfer-coding decoder");
    }
}

transfer_coding_decoder::~transfer_coding_decoder() {
    (void)inflateEnd(&stream_);
}

HttpTransferCodingDecodeResult transfer_coding_decoder::decode(
    std::string_view input, std::span<char> output_buffer) noexcept {
    if (std::holds_alternative<decoder_failed>(state_)) {
        return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(0));
    }
    if (const auto* failure = std::get_if<HttpTransferCodingDecodeError>(&state_)) {
        return fail(0, *failure);
    }
    if (std::holds_alternative<completed>(state_)) {
        return input.empty() ? complete(0) : fail(0, HttpTransferCodingDecodeError::kInvalidContent);
    }
    if (output_buffer.empty()) {
        return fail_decoder(0);
    }

    std::size_t consumed = 0;
    std::size_t produced = 0;
    for (;;) {
        if (std::holds_alternative<gzip_member_boundary>(state_)) {
            if (consumed == input.size()) {
                return produced != 0
                           ? output(consumed, std::string_view(output_buffer.data(), produced))
                           : need_input(consumed);
            }
            if (inflateReset(&stream_) != Z_OK) {
                return fail_decoder(consumed);
            }
            state_.emplace<active>();
        }

        const auto step = inflate_step(input.substr(consumed), output_buffer.subspan(produced));
        consumed += step.consumed;
        if (body_limit_.additionExceeds(decoded_bytes_, step.produced)) {
            return fail(consumed, HttpTransferCodingDecodeError::kDecodedSizeExceeded);
        }
        decoded_bytes_ += step.produced;
        produced += step.produced;

        if (step.status == Z_STREAM_END) {
            if (coding_ != HttpTransferCoding::kGzip) {
                if (consumed != input.size()) {
                    return fail(consumed, HttpTransferCodingDecodeError::kInvalidContent);
                }
                state_.emplace<completed>();
                return produced != 0
                           ? output(consumed, std::string_view(output_buffer.data(), produced))
                           : complete(consumed);
            }

            // RFC 1952 gzip data is a series of members. A member boundary is
            // not the end of the transfer coding: another member can arrive in
            // the same HTTP chunk or in a later one. Only framing EOF, reported
            // through finish_input(), commits this boundary as complete.
            state_.emplace<gzip_member_boundary>();
            if (produced == output_buffer.size()) {
                return output(consumed, std::string_view(output_buffer.data(), produced));
            }
            continue;
        }

        if (step.status != Z_OK && step.status != Z_BUF_ERROR) {
            if (step.status == Z_DATA_ERROR || step.status == Z_NEED_DICT) {
                return fail(consumed, HttpTransferCodingDecodeError::kInvalidContent);
            }
            return fail_decoder(consumed);
        }

        if (produced != 0) {
            return output(consumed, std::string_view(output_buffer.data(), produced));
        }
        if (consumed != input.size()) {
            return fail(consumed, HttpTransferCodingDecodeError::kInvalidContent);
        }
        return need_input(consumed);
    }
}

HttpTransferCodingDecodeResult transfer_coding_decoder::finish_input() noexcept {
    if (std::holds_alternative<decoder_failed>(state_)) {
        return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(0));
    }
    if (std::holds_alternative<completed>(state_) ||
        std::holds_alternative<gzip_member_boundary>(state_)) {
        state_.emplace<completed>();
        return complete(0);
    }
    if (const auto* failure = std::get_if<HttpTransferCodingDecodeError>(&state_)) {
        return fail(0, *failure);
    }
    return fail(0, HttpTransferCodingDecodeError::kInvalidContent);
}

transfer_coding_decoder::inflate_step_result transfer_coding_decoder::inflate_step(
    std::string_view input, std::span<char> output) noexcept {
    const auto input_bytes = std::min<std::size_t>(input.size(), (std::numeric_limits<uInt>::max)());
    stream_.next_in =
        input_bytes == 0 ? Z_NULL : reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream_.avail_in = static_cast<uInt>(input_bytes);
    const auto output_bytes =
        std::min<std::size_t>(output.size(), (std::numeric_limits<uInt>::max)());
    stream_.next_out = reinterpret_cast<Bytef*>(output.data());
    stream_.avail_out = static_cast<uInt>(output_bytes);

    const auto status = inflate(&stream_, Z_NO_FLUSH);
    const auto consumed = input_bytes - stream_.avail_in;
    const auto produced = output_bytes - stream_.avail_out;
    stream_.next_in = Z_NULL;
    stream_.avail_in = 0;
    stream_.next_out = Z_NULL;
    stream_.avail_out = 0;
    return inflate_step_result{consumed, produced, status};
}

HttpTransferCodingDecodeResult transfer_coding_decoder::need_input(std::size_t consumed) noexcept {
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeNeedInput(consumed));
}

HttpTransferCodingDecodeResult transfer_coding_decoder::output(
    std::size_t consumed, std::string_view bytes) noexcept {
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeOutputView(consumed, bytes));
}

HttpTransferCodingDecodeResult transfer_coding_decoder::complete(std::size_t consumed) noexcept {
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeComplete(consumed));
}

HttpTransferCodingDecodeResult transfer_coding_decoder::fail(
    std::size_t consumed, HttpTransferCodingDecodeError error) noexcept {
    state_.emplace<HttpTransferCodingDecodeError>(error);
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecodeFailure(consumed, error));
}

HttpTransferCodingDecodeResult transfer_coding_decoder::fail_decoder(std::size_t consumed) noexcept {
    state_.emplace<decoder_failed>();
    return HttpTransferCodingDecodeResult(HttpTransferCodingDecoderFailure(consumed));
}

voidpf transfer_coding_decoder::zalloc_thunk(voidpf opaque, uInt items, uInt size) noexcept {
    auto* self = static_cast<transfer_coding_decoder*>(opaque);
    return self == nullptr ? nullptr : detail::zlibPmrAllocate(self->resource_, items, size);
}

void transfer_coding_decoder::zfree_thunk(voidpf, voidpf address) noexcept {
    detail::zlibPmrFree(address);
}

}  // namespace detail

namespace {

constexpr std::size_t k_transfer_coding_scratch_bytes = 4096;

struct transfer_coding_stage final {
    transfer_coding_stage(HttpTransferCoding coding, std::pmr::memory_resource* resource,
        ProtocolByteLimit decoded_limit)
        : decoder(coding, resource, decoded_limit) {}

    detail::transfer_coding_decoder decoder;
    std::size_t begin{0};
    std::size_t end{0};
    bool complete{false};
};

// Stable, exact-sized stage storage. The constructed prefix is recorded before
// starting the next stage, so failure at any zlib allocation is handled by RAII.
struct transfer_stages_deleter final {
    std::pmr::memory_resource* resource;
    std::size_t capacity;
    std::size_t constructed{0};
    void operator()(transfer_coding_stage* stages) const noexcept {
        std::destroy_n(stages, constructed);
        std::pmr::polymorphic_allocator<transfer_coding_stage>(resource)
            .deallocate(stages, capacity);
    }
};

using transfer_stages_owner = std::unique_ptr<transfer_coding_stage, transfer_stages_deleter>;

[[nodiscard]] transfer_stages_owner make_transfer_stages(
    std::span<const HttpTransferCoding> codings, std::pmr::memory_resource* resource,
    ProtocolByteLimit decoded_limit) {
    if (codings.empty() || codings.size() > kMaxTransferCodings) {
        throw std::invalid_argument("invalid transfer-coding stack length");
    }
    std::pmr::polymorphic_allocator<transfer_coding_stage> allocator(resource);
    transfer_stages_owner owner(allocator.allocate(codings.size()), {resource, codings.size()});
    for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
        auto& count = owner.get_deleter().constructed;
        std::construct_at(owner.get() + count, *coding, resource, decoded_limit);
        ++count;
    }
    return owner;
}

}  // namespace

struct http_transfer_coding_stack_decoder::impl final {
    impl(std::span<const HttpTransferCoding> codings, std::pmr::memory_resource* resource,
        ProtocolByteLimit decoded_limit)
        : resource(detail::httpPmrResourceOrDefault(resource)),
          stage_storage(make_transfer_stages(codings, this->resource, decoded_limit)),
          stages(stage_storage.get(), codings.size()),
          buffers(codings.size() - 1, this->resource) {}

    std::pmr::memory_resource* resource;
    transfer_stages_owner stage_storage;
    std::span<transfer_coding_stage> stages;
    // Only inter-stage edges own scratch. The final stage writes directly to
    // the caller's buffer, so a one-coding sequence has no internal scratch.
    std::pmr::vector<std::array<char, k_transfer_coding_scratch_bytes>> buffers;
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
                layer_input = std::string_view(impl_->buffers[index - 1].data() + previous.begin,
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
                                    : std::span<char>(impl_->buffers[index]);
            const auto decoded = stage.decoder.decode(layer_input, output);
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
        const auto finished = stage.decoder.finish_input();
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
