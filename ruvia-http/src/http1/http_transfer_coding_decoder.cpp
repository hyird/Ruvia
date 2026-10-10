#include "ruvia/http/http_transfer_coding_decoder.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>

#include "ruvia/http/detail/util/pmr_resource.h"

#include "coding/zlib_pmr_allocation.h"

namespace ruvia {

namespace detail {

// One zlib stage is an implementation detail of the coding sequence, not a
// second public decoder. Its address must stay fixed for zlib's opaque pointer.
class transfer_coding_decoder final {
public:
    transfer_coding_decoder(http_transfer_coding coding, std::pmr::memory_resource* resource, protocol_byte_limit decoded_limit);
    ~transfer_coding_decoder();
    transfer_coding_decoder(const transfer_coding_decoder&) = delete;
    transfer_coding_decoder& operator=(const transfer_coding_decoder&) = delete;
    [[nodiscard]] http_transfer_coding_decode_result decode(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] http_transfer_coding_decode_result finish_input() noexcept;

private:
    struct inflate_step_result {
        std::size_t consumed_{0};
        std::size_t produced_{0};
        int status_{Z_OK};
    };
    struct active final {};
    struct gzip_member_boundary final {};
    struct completed final {};
    struct decoder_failed final {};
    using state = std::variant<active, gzip_member_boundary, completed, http_transfer_coding_decode_error, decoder_failed>;
    [[nodiscard]] inflate_step_result inflate_step(std::string_view input, std::span<char> output) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result need_input(std::size_t consumed) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result output(std::size_t consumed, std::string_view bytes) noexcept;
    [[nodiscard]] static http_transfer_coding_decode_result complete(std::size_t consumed) noexcept;
    [[nodiscard]] http_transfer_coding_decode_result fail(std::size_t consumed, http_transfer_coding_decode_error error) noexcept;
    [[nodiscard]] http_transfer_coding_decode_result fail_decoder(std::size_t consumed) noexcept;
    static voidpf zalloc_thunk(voidpf opaque, uInt items, uInt size) noexcept;
    static void zfree_thunk(voidpf opaque, voidpf address) noexcept;
    z_stream stream_{};
    state state_{active{}};
    std::pmr::memory_resource* resource_;
    protocol_byte_limit body_limit_;
    std::size_t decoded_bytes_{0};
    // An empty coded body carries no content and completes at framing EOF.
    bool received_input_{false};
    http_transfer_coding coding_;
};

transfer_coding_decoder::transfer_coding_decoder(
    http_transfer_coding coding, std::pmr::memory_resource* resource, protocol_byte_limit decoded_limit)
    : resource_(detail::http_pmr_resource_or_default(resource)),
      body_limit_(decoded_limit),
      coding_(coding) {
    int window_bits = 0;
    switch (coding) {
        case http_transfer_coding::gzip:
            window_bits = 15 + 16;
            break;
        case http_transfer_coding::deflate:
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

http_transfer_coding_decode_result transfer_coding_decoder::decode(
    std::string_view input, std::span<char> output_buffer) noexcept {
    if (std::holds_alternative<decoder_failed>(state_)) {
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }
    if (const auto* failure = std::get_if<http_transfer_coding_decode_error>(&state_)) {
        return fail(0, *failure);
    }
    if (std::holds_alternative<completed>(state_)) {
        return input.empty() ? complete(0) : fail(0, http_transfer_coding_decode_error::invalid_content);
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
        consumed += step.consumed_;
        received_input_ = received_input_ || step.consumed_ != 0;
        if (body_limit_.addition_exceeds(decoded_bytes_, step.produced_)) {
            return fail(consumed, http_transfer_coding_decode_error::decoded_size_exceeded);
        }
        decoded_bytes_ += step.produced_;
        produced += step.produced_;

        if (step.status_ == Z_STREAM_END) {
            if (coding_ != http_transfer_coding::gzip) {
                if (consumed != input.size()) {
                    return fail(consumed, http_transfer_coding_decode_error::invalid_content);
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

        if (step.status_ != Z_OK && step.status_ != Z_BUF_ERROR) {
            if (step.status_ == Z_DATA_ERROR || step.status_ == Z_NEED_DICT) {
                return fail(consumed, http_transfer_coding_decode_error::invalid_content);
            }
            return fail_decoder(consumed);
        }

        if (produced != 0) {
            return output(consumed, std::string_view(output_buffer.data(), produced));
        }
        if (consumed != input.size()) {
            return fail(consumed, http_transfer_coding_decode_error::invalid_content);
        }
        return need_input(consumed);
    }
}

http_transfer_coding_decode_result transfer_coding_decoder::finish_input() noexcept {
    if (std::holds_alternative<decoder_failed>(state_)) {
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }
    if (std::holds_alternative<completed>(state_) ||
        std::holds_alternative<gzip_member_boundary>(state_) ||
        (std::holds_alternative<active>(state_) && !received_input_)) {
        state_.emplace<completed>();
        return complete(0);
    }
    if (const auto* failure = std::get_if<http_transfer_coding_decode_error>(&state_)) {
        return fail(0, *failure);
    }
    return fail(0, http_transfer_coding_decode_error::invalid_content);
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

http_transfer_coding_decode_result transfer_coding_decoder::need_input(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(consumed));
}

http_transfer_coding_decode_result transfer_coding_decoder::output(
    std::size_t consumed, std::string_view bytes_value) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_output_view(consumed, bytes_value));
}

http_transfer_coding_decode_result transfer_coding_decoder::complete(std::size_t consumed) noexcept {
    return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(consumed));
}

http_transfer_coding_decode_result transfer_coding_decoder::fail(
    std::size_t consumed, http_transfer_coding_decode_error error) noexcept {
    state_.emplace<http_transfer_coding_decode_error>(error);
    return http_transfer_coding_decode_result(http_transfer_coding_decode_failure(consumed, error));
}

http_transfer_coding_decode_result transfer_coding_decoder::fail_decoder(std::size_t consumed) noexcept {
    state_.emplace<decoder_failed>();
    return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(consumed));
}

voidpf transfer_coding_decoder::zalloc_thunk(voidpf opaque, uInt items, uInt size) noexcept {
    auto* self = static_cast<transfer_coding_decoder*>(opaque);
    return self == nullptr ? nullptr : detail::zlib_pmr_allocate(self->resource_, items, size);
}

void transfer_coding_decoder::zfree_thunk(voidpf, voidpf address) noexcept {
    detail::zlib_pmr_free(address);
}

}  // namespace detail

namespace {

constexpr std::size_t k_transfer_coding_scratch_bytes = 4096;

struct transfer_coding_stage final {
    transfer_coding_stage(http_transfer_coding coding, std::pmr::memory_resource* resource,
        protocol_byte_limit decoded_limit)
        : decoder_(coding, resource, decoded_limit) {}

    detail::transfer_coding_decoder decoder_;
    std::size_t begin_{0};
    std::size_t end_{0};
    bool complete_{false};
};

// Stable, exact-sized stage storage. The constructed prefix is recorded before
// starting the next stage, so failure at any zlib allocation is handled by RAII.
struct transfer_stages_deleter final {
    std::pmr::memory_resource* resource_;
    std::size_t capacity_;
    std::size_t constructed_{0};
    void operator()(transfer_coding_stage* stages) const noexcept {
        std::destroy_n(stages, constructed_);
        std::pmr::polymorphic_allocator<transfer_coding_stage>(resource_)
            .deallocate(stages, capacity_);
    }
};

using transfer_stages_owner = std::unique_ptr<transfer_coding_stage, transfer_stages_deleter>;

[[nodiscard]] transfer_stages_owner make_transfer_stages(
    std::span<const http_transfer_coding> codings, std::pmr::memory_resource* resource,
    protocol_byte_limit decoded_limit) {
    if (codings.empty() || codings.size() > max_transfer_codings) {
        throw std::invalid_argument("invalid transfer-coding stack length");
    }
    std::pmr::polymorphic_allocator<transfer_coding_stage> allocator(resource);
    transfer_stages_owner owner_value(allocator.allocate(codings.size()), {resource, codings.size()});
    for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
        auto& count = owner_value.get_deleter().constructed_;
        std::construct_at(owner_value.get() + count, *coding, resource, decoded_limit);
        ++count;
    }
    return owner_value;
}

}  // namespace

struct http_transfer_coding_stack_decoder::impl final {
    impl(std::span<const http_transfer_coding> codings, std::pmr::memory_resource* resource,
        protocol_byte_limit decoded_limit)
        : resource_(detail::http_pmr_resource_or_default(resource)),
          stage_storage_(make_transfer_stages(codings, this->resource_, decoded_limit)),
          stages_(stage_storage_.get(), codings.size()),
          buffers_(codings.size() - 1, this->resource_) {}

    std::pmr::memory_resource* resource_;
    transfer_stages_owner stage_storage_;
    std::span<transfer_coding_stage> stages_;
    // Only inter-stage edges own scratch. The final stage writes directly to
    // the caller's buffer, so a one-coding sequence has no internal scratch.
    std::pmr::vector<std::array<char, k_transfer_coding_scratch_bytes>> buffers_;
    std::optional<http_transfer_coding_decode_error> failure_;
    bool decoder_failed_{false};
};

http_transfer_coding_stack_decoder::http_transfer_coding_stack_decoder(
    std::span<const http_transfer_coding> codings, std::pmr::memory_resource* resource,
    protocol_byte_limit decoded_limit) {
    auto* memory = detail::http_pmr_resource_or_default(resource);
    std::pmr::polymorphic_allocator<impl> allocator(memory);
    impl_ = allocator.new_object<impl>(codings, memory, decoded_limit);
}

http_transfer_coding_stack_decoder::~http_transfer_coding_stack_decoder() {
    if (impl_ != nullptr) {
        std::pmr::polymorphic_allocator<impl>(impl_->resource_).delete_object(impl_);
    }
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::decode(
    std::string_view input, std::span<char> output_buffer) noexcept {
    const auto failed = [this](std::size_t consumed) {
        if (impl_->decoder_failed_) {
            return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(consumed));
        }
        return http_transfer_coding_decode_result(
            http_transfer_coding_decode_failure(consumed, *impl_->failure_));
    };
    if (impl_->decoder_failed_ || impl_->failure_.has_value()) {
        return failed(0);
    }
    if (output_buffer.empty()) {
        impl_->decoder_failed_ = true;
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }

    std::size_t wire_consumed = 0;
    for (;;) {
        bool progressed = false;
        for (std::size_t index = 0; index < impl_->stages_.size(); ++index) {
            auto& stage = impl_->stages_[index];
            if (index + 1 < impl_->stages_.size() && stage.begin_ != stage.end_) {
                continue;
            }
            std::string_view layer_input;
            if (index == 0) {
                layer_input = input.substr(wire_consumed);
            } else {
                auto& previous = impl_->stages_[index - 1];
                layer_input = std::string_view(impl_->buffers_[index - 1].data() + previous.begin_,
                    previous.end_ - previous.begin_);
            }
            if (stage.complete_) {
                if (!layer_input.empty()) {
                    impl_->failure_ = http_transfer_coding_decode_error::invalid_content;
                    return failed(wire_consumed);
                }
                continue;
            }
            const auto output = index + 1 == impl_->stages_.size()
                                    ? output_buffer
                                    : std::span<char>(impl_->buffers_[index]);
            const auto decoded = stage.decoder_.decode(layer_input, output);
            const auto consumed = decoded.consumed_bytes();
            if (index == 0) {
                wire_consumed += consumed;
            } else {
                auto& previous = impl_->stages_[index - 1];
                previous.begin_ += consumed;
                if (previous.begin_ == previous.end_) {
                    previous.begin_ = 0;
                    previous.end_ = 0;
                }
            }
            progressed = progressed || consumed != 0;
            if (const auto* bytes = decoded.output()) {
                progressed = true;
                if (index + 1 == impl_->stages_.size()) {
                    return http_transfer_coding_decode_result(
                        http_transfer_coding_decode_output_view(wire_consumed, bytes->bytes()));
                }
                stage.begin_ = 0;
                stage.end_ = bytes->bytes().size();
                continue;
            }
            if (const auto* protocol_failure = decoded.failure()) {
                impl_->failure_ = protocol_failure->error();
                return failed(wire_consumed);
            }
            if (decoded.decoder_failure() != nullptr) {
                impl_->decoder_failed_ = true;
                return failed(wire_consumed);
            }
            if (decoded.complete() != nullptr) {
                stage.complete_ = true;
                progressed = true;
            } else if (decoded.need_input() == nullptr) {
                impl_->decoder_failed_ = true;
                return failed(wire_consumed);
            }
        }

        bool all_complete = true;
        bool pending = false;
        for (std::size_t index = 0; index < impl_->stages_.size(); ++index) {
            const auto& stage = impl_->stages_[index];
            all_complete = all_complete && stage.complete_;
            pending = pending || (index + 1 < impl_->stages_.size() && stage.begin_ != stage.end_);
        }
        if (all_complete && !pending) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(wire_consumed));
        }
        if (!progressed) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(wire_consumed));
        }
    }
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::finish_input() noexcept {
    if (impl_->decoder_failed_) {
        return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
    }
    if (impl_->failure_.has_value()) {
        return http_transfer_coding_decode_result(
            http_transfer_coding_decode_failure(0, *impl_->failure_));
    }
    for (std::size_t index = 0; index < impl_->stages_.size(); ++index) {
        auto& stage = impl_->stages_[index];
        if (index + 1 < impl_->stages_.size() && stage.begin_ != stage.end_) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(0));
        }
        if (stage.complete_) {
            continue;
        }
        const auto finished = stage.decoder_.finish_input();
        if (const auto* protocol_failure = finished.failure()) {
            impl_->failure_ = protocol_failure->error();
            return http_transfer_coding_decode_result(
                http_transfer_coding_decode_failure(0, *impl_->failure_));
        }
        if (finished.decoder_failure() != nullptr) {
            impl_->decoder_failed_ = true;
            return http_transfer_coding_decode_result(http_transfer_coding_decoder_failure(0));
        }
        if (finished.complete() == nullptr) {
            return http_transfer_coding_decode_result(http_transfer_coding_decode_need_input(0));
        }
        stage.complete_ = true;
    }
    return http_transfer_coding_decode_result(http_transfer_coding_decode_complete(0));
}

}  // namespace ruvia
