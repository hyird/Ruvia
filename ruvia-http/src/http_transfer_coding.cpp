#include "http_transfer_coding.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "http_compression_codec.h"
#include "http_content_codec.h"

namespace ruvia {

namespace {

constexpr std::size_t transfer_coding_scratch_bytes = 4096;

struct transfer_coding_scratch_buffer final {
    // Stage output bounds valid bytes to [begin_, end_), so default construction
    // leaves the 4 KiB storage uninitialized instead of zero-filling it.
    std::array<char, transfer_coding_scratch_bytes> bytes_;
};

[[nodiscard]] http_content_coding checked_transfer_coding(http_content_coding coding) {
    if (!is_http_transfer_coding(coding)) {
        throw std::invalid_argument("unsupported HTTP transfer coding");
    }
    return coding;
}

struct transfer_coding_stage final {
    transfer_coding_stage(http_content_coding coding, protocol_byte_limit decoded_limit, http_codec_memory* memory) :
        decoder_(checked_transfer_coding(coding), {.max_decoded_bytes_ = decoded_limit.read_ceiling(), .memory_ = memory}) {}

    detail::http_content_decoder_stream decoder_;
    std::size_t begin_{0};
    std::size_t end_{0};
    bool complete_{false};
    bool received_input_{false};
};

}  // namespace

struct http_transfer_coding_stack_decoder::impl_type final {
    impl_type(std::span<const http_content_coding> codings, protocol_byte_limit decoded_limit, http_codec_memory* memory) :
        stage_count_(codings.size()) {
        std::size_t index = 0;
        for (auto coding = codings.rbegin(); coding != codings.rend(); ++coding) {
            stages_[index++].emplace(*coding, decoded_limit, memory);
        }
    }
    std::array<std::optional<transfer_coding_stage>, max_content_codings> stages_{};
    std::size_t stage_count_{0};
    // Only inter-stage edges own scratch. The final stage writes directly to
    // the caller's buffer, so a one-coding sequence uses no internal scratch.
    std::array<transfer_coding_scratch_buffer, max_content_codings - 1> buffers_;
    std::optional<http_content_decode_error> failure_;
};

http_transfer_coding_stack_decoder::http_transfer_coding_stack_decoder(std::span<const http_content_coding> codings, protocol_byte_limit decoded_limit, http_codec_memory* memory) {
    if (codings.empty() || codings.size() > max_content_codings) {
        throw std::invalid_argument("invalid transfer-coding stack length");
    }
    impl_ = new impl_type(codings, decoded_limit, memory);
}

http_transfer_coding_stack_decoder::~http_transfer_coding_stack_decoder() {
    delete impl_;
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::decode(std::string_view input, std::span<char> output_buffer) {
    const auto failed = [this](std::size_t consumed) {
        return http_transfer_coding_decode_result(consumed, http_transfer_coding_decode_failure(*impl_->failure_));
    };
    if (impl_->failure_.has_value()) {
        return failed(0);
    }
    if (state() == decoder_state::finished) {
        return http_transfer_coding_decode_result(0, decode_complete{});
    }
    if (output_buffer.empty()) {
        throw std::invalid_argument("transfer-coding decode needs output space");
    }

    std::size_t wire_consumed = 0;
    for (;;) {
        bool progressed = false;
        for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
            auto& stage = *impl_->stages_[index];
            if (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_) {
                continue;
            }
            std::string_view layer_input;
            if (index == 0) {
                layer_input = input.substr(wire_consumed);
            } else {
                auto& previous = *impl_->stages_[index - 1];
                layer_input = std::string_view(impl_->buffers_[index - 1].bytes_.data() + previous.begin_, previous.end_ - previous.begin_);
            }
            if (stage.complete_) {
                if (!layer_input.empty()) {
                    impl_->failure_ = http_content_decode_error::invalid_content;
                    return failed(wire_consumed);
                }
                continue;
            }
            const auto output = index + 1 == impl_->stage_count_ ? output_buffer : std::span<char>(impl_->buffers_[index].bytes_);
            const auto decoded = stage.decoder_.decode(layer_input, output);
            const auto consumed = decoded.consumed_bytes();
            stage.received_input_ = stage.received_input_ || consumed != 0;
            if (index == 0) {
                wire_consumed += consumed;
            } else {
                auto& previous = *impl_->stages_[index - 1];
                previous.begin_ += consumed;
                if (previous.begin_ == previous.end_) {
                    previous.begin_ = 0;
                    previous.end_ = 0;
                }
            }
            progressed = progressed || consumed != 0;
            if (const auto* bytes = decoded.get_if<decode_output>()) {
                progressed = true;
                if (index + 1 == impl_->stage_count_) {
                    return http_transfer_coding_decode_result(wire_consumed, decode_output(bytes->bytes()));
                }
                stage.begin_ = 0;
                stage.end_ = bytes->bytes().size();
                continue;
            }
            if (const auto* protocol_failure = decoded.get_if<http_transfer_coding_decode_failure>()) {
                impl_->failure_ = protocol_failure->error();
                return failed(wire_consumed);
            }
            if (decoded.holds<decode_complete>()) {
                stage.complete_ = true;
                progressed = true;
            }
        }

        bool all_complete = true;
        bool pending = false;
        for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
            const auto& stage = *impl_->stages_[index];
            all_complete = all_complete && stage.complete_;
            pending = pending || (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_);
        }
        if (all_complete && !pending) {
            return http_transfer_coding_decode_result(wire_consumed, decode_complete{});
        }
        if (!progressed) {
            return http_transfer_coding_decode_result(wire_consumed, decode_need_input{});
        }
    }
}

decoder_state http_transfer_coding_stack_decoder::state() const noexcept {
    if (impl_->failure_.has_value()) {
        return decoder_state::failed;
    }
    for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
        const auto& stage = *impl_->stages_[index];
        if (!stage.complete_ || stage.begin_ != stage.end_) {
            return decoder_state::active;
        }
    }
    return decoder_state::finished;
}

void http_transfer_coding_stack_decoder::reset() {
    for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
        auto& stage = *impl_->stages_[index];
        stage.decoder_.reset();
        stage.begin_ = 0;
        stage.end_ = 0;
        stage.complete_ = false;
        stage.received_input_ = false;
    }
    impl_->failure_.reset();
}

http_transfer_coding_decode_result http_transfer_coding_stack_decoder::finish_input() noexcept {
    if (impl_->failure_.has_value()) {
        return http_transfer_coding_decode_result(0, http_transfer_coding_decode_failure(*impl_->failure_));
    }
    const bool wire_received_input = impl_->stages_[0]->received_input_;
    for (std::size_t index = 0; index < impl_->stage_count_; ++index) {
        auto& stage = *impl_->stages_[index];
        if (index + 1 < impl_->stage_count_ && stage.begin_ != stage.end_) {
            return http_transfer_coding_decode_result(0, decode_need_input{});
        }
        if (stage.complete_) {
            continue;
        }
        if (index > 0 && wire_received_input && !stage.received_input_) {
            impl_->failure_ = http_content_decode_error::invalid_content;
            return http_transfer_coding_decode_result(0, http_transfer_coding_decode_failure(*impl_->failure_));
        }
        const auto finished = stage.decoder_.finish_input();
        if (const auto* protocol_failure = finished.get_if<http_transfer_coding_decode_failure>()) {
            impl_->failure_ = protocol_failure->error();
            return http_transfer_coding_decode_result(0, http_transfer_coding_decode_failure(*impl_->failure_));
        }
        stage.complete_ = true;
    }
    return http_transfer_coding_decode_result(0, decode_complete{});
}

}  // namespace ruvia
