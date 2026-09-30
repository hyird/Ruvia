#include "ruvia/http/Http1ClientResponseBodyDecoder.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace ruvia {

std::string_view http1ClientResponseBodyErrorMessage(Http1ClientResponseBodyError error) noexcept {
    switch (error) {
        case Http1ClientResponseBodyError::kInvalidFraming:
            return "invalid HTTP/1 response body framing";
        case Http1ClientResponseBodyError::kInvalidTransferCoding:
            return "invalid HTTP/1 response transfer coding";
        case Http1ClientResponseBodyError::kIncompleteBody:
            return "incomplete HTTP/1 response body";
        case Http1ClientResponseBodyError::kNonEmpty205:
            return "HTTP 205 response content is not empty";
    }
    return "invalid HTTP/1 response body";
}

Http1ClientResponseBodyDecoder::Http1ClientResponseBodyDecoder(
    Http1ClientResponsePlan plan, std::pmr::memory_resource* resource) {
    if (resource == nullptr) {
        throw std::invalid_argument("HTTP/1 response body decoder requires a memory resource");
    }
    if (plan.informational() || plan.connectTunnel() || plan.protocolUpgrade()) {
        throw std::invalid_argument("HTTP/1 response plan has no ordinary message body");
    }
    const auto configureCoding = [this, resource](HttpTransferCodings codings) {
        if (codings.count > 1) {
            throw std::invalid_argument("multiple HTTP transfer codings are unsupported");
        }
        if (codings.count == 1) {
            transfer_.emplace(codings.values[0], resource, ProtocolByteLimit::unlimited());
        }
    };
    if (const auto* without = plan.withoutContent()) {
        framing_ = Framing::kNoBody;
        persistence_ = without->persistence();
    } else if (const auto* known = plan.knownLength()) {
        framing_ = Framing::kFixed;
        remaining_ = known->contentLength();
        persistence_ = known->persistence();
    } else if (const auto* chunked = plan.chunked()) {
        framing_ = Framing::kChunked;
        persistence_ = chunked->persistence();
        configureCoding(chunked->transferCodings());
        chunked_.emplace(ProtocolByteLimit::unlimited());
    } else if (const auto* close = plan.closeDelimited()) {
        framing_ = Framing::kCloseDelimited;
        configureCoding(close->transferCodings());
    } else if (const auto* zero = plan.zeroContent()) {
        zeroContent_ = true;
        if (const auto* zeroKnown = zero->knownLength()) {
            framing_ = Framing::kFixed;
            remaining_ = zeroKnown->contentLength();
            persistence_ = zeroKnown->persistence();
        } else if (const auto* zeroChunked = zero->chunked()) {
            framing_ = Framing::kChunked;
            persistence_ = zeroChunked->persistence();
            configureCoding(zeroChunked->transferCodings());
            chunked_.emplace(ProtocolByteLimit::unlimited());
        } else if (const auto* zeroClose = zero->closeDelimited()) {
            framing_ = Framing::kCloseDelimited;
            configureCoding(zeroClose->transferCodings());
        } else {
            throw std::invalid_argument("invalid zero-content HTTP/1 response plan");
        }
    } else {
        throw std::invalid_argument("unsupported HTTP/1 response plan");
    }
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::decode(
    std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replayTerminal();
    }
    if (eof_) {
        return fail(Http1ClientResponseBodyError::kIncompleteBody);
    }
    return step(available, scratch, false);
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::finishInput(
    std::string_view available, std::span<char> scratch) {
    if (scratch.empty()) {
        throw std::invalid_argument("HTTP/1 response body decoder scratch must be non-empty");
    }
    if (terminal_) {
        return replayTerminal();
    }
    eof_ = true;
    return step(available, scratch, true);
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::step(
    std::string_view available, std::span<char> scratch, bool eof) {
    if (terminal_) {
        return replayTerminal();
    }
    if (trailersReported_) {
        if (transfer_) {
            return finishTransfer(0, scratch);
        }
        return complete(0);
    }

    std::size_t consumed = 0;
    for (;;) {
        // A full output step can leave inflate output buffered after consuming
        // all wire input. Drain it before asking framing or transport for more.
        if (transfer_ && transferPhase_ == TransferPhase::kDrainOutput) {
            const auto pending = driveTransfer({}, consumed, scratch);
            if (pending.output() || pending.protocolFailure() || pending.decoderFailure()) {
                return pending;
            }
        }
        if (pendingBodyBytes_ != 0) {
            const auto input = available.substr(consumed,
                std::min(pendingBodyBytes_, available.size() - consumed));
            const auto driven = driveTransfer(input, consumed, scratch);
            const auto inputConsumed = driven.consumedBytes() - consumed;
            pendingBodyBytes_ -= inputConsumed;
            consumed += inputConsumed;
            if (driven.output() || driven.protocolFailure() || driven.decoderFailure()) {
                if (pendingBodyBytes_ == 0) {
                    const auto delimiter = std::min(pendingDelimiterBytes_, available.size() - consumed);
                    consumed += delimiter;
                    pendingDelimiterBytes_ -= delimiter;
                }
                return std::visit([consumed](const auto& payload) -> Result {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<T, OutputView>) {
                        return Result(OutputView(consumed, payload.bytes()));
                    } else if constexpr (std::is_same_v<T, ProtocolFailure>) {
                        return Result(ProtocolFailure(consumed, payload.error()));
                    } else if constexpr (std::is_same_v<T, DecoderFailure>) {
                        return Result(DecoderFailure(consumed));
                    } else {
                        return Result(payload);
                    }
                },
                    driven.value_);
            }
            if (pendingBodyBytes_ != 0) {
                if (eof) {
                    return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                }
                return needInput(consumed);
            }
            if (pendingDelimiterBytes_ != 0) {
                const auto delimiter = std::min(pendingDelimiterBytes_, available.size() - consumed);
                consumed += delimiter;
                pendingDelimiterBytes_ -= delimiter;
                if (pendingDelimiterBytes_ != 0) {
                    if (eof) {
                        return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                    }
                    return needInput(consumed);
                }
            }
            if (driven.needInput() && consumed == available.size()) {
                if (eof) {
                    if (framing_ == Framing::kChunked) {
                        return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                    }
                    if (framing_ == Framing::kFixed && remaining_ == 0) {
                        return finishTransfer(consumed, scratch);
                    }
                    if (framing_ == Framing::kCloseDelimited) {
                        return finishTransfer(consumed, scratch);
                    }
                    return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                }
                return needInput(consumed);
            }
            continue;
        }

        if (pendingDelimiterBytes_ != 0) {
            const auto delimiter = std::min(pendingDelimiterBytes_, available.size() - consumed);
            consumed += delimiter;
            pendingDelimiterBytes_ -= delimiter;
            if (pendingDelimiterBytes_ != 0) {
                if (eof) {
                    return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                }
                return needInput(consumed);
            }
        }

        const auto rest = available.substr(consumed);
        switch (framing_) {
            case Framing::kNoBody:
                return complete(consumed);
            case Framing::kFixed: {
                if (remaining_ == 0) {
                    if (transfer_) {
                        return finishTransfer(consumed, scratch);
                    }
                    return complete(consumed);
                }
                if (rest.empty()) {
                    if (eof) {
                        return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                    }
                    return needInput(consumed);
                }
                const auto inputLength = std::min(remaining_, rest.size());
                if (transfer_) {
                    const auto driven = driveTransfer(rest.substr(0, inputLength), consumed, scratch);
                    const auto inputConsumed = driven.consumedBytes() - consumed;
                    remaining_ -= inputConsumed;
                    if (driven.output() || driven.protocolFailure() || driven.decoderFailure()) {
                        return driven;
                    }
                    consumed += inputConsumed;
                    if (remaining_ == 0) {
                        if (driven.needInput()) {
                            return finishTransfer(consumed, scratch);
                        }
                    }
                    if (inputConsumed == 0 || consumed == available.size()) {
                        if (eof && remaining_ != 0) {
                            return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                        }
                        return needInput(consumed);
                    }
                    continue;
                }
                const auto outputLength = std::min(inputLength, scratch.size());
                if (zeroContent_ && outputLength != 0) {
                    return fail(Http1ClientResponseBodyError::kNonEmpty205, consumed + outputLength);
                }
                remaining_ -= outputLength;
                return emit(consumed + outputLength, rest.substr(0, outputLength));
            }
            case Framing::kCloseDelimited: {
                if (transfer_) {
                    if (transferPhase_ == TransferPhase::kEnded) {
                        if (!rest.empty()) {
                            return fail(Http1ClientResponseBodyError::kInvalidTransferCoding, consumed);
                        }
                        if (eof) {
                            return complete(consumed);
                        }
                        return needInput(consumed);
                    }
                    if (!rest.empty()) {
                        const auto driven = driveTransfer(rest, consumed, scratch);
                        const auto inputConsumed = driven.consumedBytes() - consumed;
                        consumed += inputConsumed;
                        if (driven.output() || driven.protocolFailure() || driven.decoderFailure()) {
                            return driven;
                        }
                        if (transferPhase_ == TransferPhase::kEnded) {
                            continue;
                        }
                        if (consumed < available.size() && inputConsumed != 0) {
                            continue;
                        }
                        if (consumed == available.size()) {
                            if (eof) {
                                return finishTransfer(consumed, scratch);
                            }
                            return needInput(consumed);
                        }
                        if (eof) {
                            return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                        }
                        return needInput(consumed);
                    }
                    if (eof) {
                        return finishTransfer(consumed, scratch);
                    }
                    return needInput(consumed);
                }
                if (!rest.empty()) {
                    const auto outputLength = std::min(rest.size(), scratch.size());
                    if (zeroContent_ && outputLength != 0) {
                        return fail(Http1ClientResponseBodyError::kNonEmpty205, consumed + outputLength);
                    }
                    return emit(consumed + outputLength, rest.substr(0, outputLength));
                }
                return eof ? complete(consumed) : needInput(consumed);
            }
            case Framing::kChunked: {
                const auto decoded = chunked_->decode(rest,
                    transfer_ ? (std::numeric_limits<std::size_t>::max)() : scratch.size());
                const auto childConsumed = decoded.consumedBytes();
                if (decoded.state() == HttpResponseChunkedBodyDecoder::State::kInvalid) {
                    return fail(Http1ClientResponseBodyError::kInvalidFraming, consumed + childConsumed);
                }
                if (decoded.state() == HttpResponseChunkedBodyDecoder::State::kNeedMore) {
                    consumed += childConsumed;
                    if (eof) {
                        return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                    }
                    return needInput(consumed);
                }
                if (decoded.state() == HttpResponseChunkedBodyDecoder::State::kComplete) {
                    trailersReported_ = true;
                    return validatedTrailers(consumed + childConsumed, decoded.trailers());
                }

                const auto body = decoded.body();
                if (!transfer_) {
                    if (zeroContent_ && !body.empty()) {
                        return fail(Http1ClientResponseBodyError::kNonEmpty205,
                            consumed + childConsumed);
                    }
                    return emit(consumed + childConsumed, body);
                }
                const auto bodyOffset = static_cast<std::size_t>(body.data() - rest.data());
                if (bodyOffset > childConsumed || body.size() > childConsumed - bodyOffset) {
                    return fail(Http1ClientResponseBodyError::kInvalidFraming, consumed + childConsumed);
                }
                const auto delimiterDebt = childConsumed - bodyOffset - body.size();
                const auto driven = driveTransfer(body, consumed + bodyOffset, scratch);
                const auto inputConsumed = driven.consumedBytes() - (consumed + bodyOffset);
                pendingBodyBytes_ = body.size() - std::min(body.size(), inputConsumed);
                pendingDelimiterBytes_ = delimiterDebt;
                const auto wireConsumed = driven.consumedBytes();
                if (pendingBodyBytes_ == 0) {
                    const auto availableDelimiter = std::min(
                        pendingDelimiterBytes_, available.size() - wireConsumed);
                    pendingDelimiterBytes_ -= availableDelimiter;
                    if (pendingDelimiterBytes_ == 0) {
                        // Delimiter was already validated by the chunk decoder.
                        if (driven.output()) {
                            return emit(wireConsumed + availableDelimiter, driven.output()->bytes());
                        }
                        if (driven.protocolFailure()) {
                            return fail(driven.protocolFailure()->error(),
                                wireConsumed + availableDelimiter);
                        }
                        if (driven.decoderFailure()) {
                            return decoderFailure(wireConsumed + availableDelimiter);
                        }
                        consumed = wireConsumed + availableDelimiter;
                        if (driven.needInput()) {
                            if (consumed == available.size()) {
                                if (eof) {
                                    return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
                                }
                                return needInput(consumed);
                            }
                            continue;
                        }
                    }
                }
                if (driven.output()) {
                    return emit(wireConsumed, driven.output()->bytes());
                }
                if (driven.protocolFailure()) {
                    return fail(driven.protocolFailure()->error(), wireConsumed);
                }
                if (driven.decoderFailure()) {
                    return decoderFailure(wireConsumed);
                }
                if (pendingBodyBytes_ == 0 && pendingDelimiterBytes_ == 0) {
                    consumed = wireConsumed;
                    continue;
                }
                if (eof) {
                    return fail(Http1ClientResponseBodyError::kIncompleteBody, wireConsumed);
                }
                return needInput(wireConsumed);
            }
        }
    }
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::driveTransfer(
    std::string_view input, std::size_t wirePrefix, std::span<char> scratch) {
    const auto decoded = transfer_->decode(input, scratch);
    const auto consumed = wirePrefix + decoded.consumedBytes();
    if (const auto* output = decoded.output()) {
        if (zeroContent_ && !output->bytes().empty()) {
            return fail(Http1ClientResponseBodyError::kNonEmpty205, consumed);
        }
        transferPhase_ = TransferPhase::kDrainOutput;
        return emit(consumed, output->bytes());
    }
    if (decoded.needInput() != nullptr) {
        transferPhase_ = TransferPhase::kNeedsFramedInput;
        return needInput(consumed);
    }
    if (decoded.complete() != nullptr) {
        transferPhase_ = TransferPhase::kEnded;
        return needInput(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(Http1ClientResponseBodyError::kInvalidTransferCoding, consumed);
    }
    return decoderFailure(consumed);
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::finishTransfer(
    std::size_t consumed, std::span<char> scratch) {
    if (!transfer_) {
        return complete(consumed);
    }
    auto decoded = transfer_->decode({}, scratch);
    if (const auto* output = decoded.output()) {
        if (zeroContent_ && !output->bytes().empty()) {
            return fail(Http1ClientResponseBodyError::kNonEmpty205, consumed);
        }
        transferPhase_ = TransferPhase::kDrainOutput;
        return emit(consumed, output->bytes());
    }
    if (decoded.decoderFailure() != nullptr) {
        return decoderFailure(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
    }
    if (decoded.needInput() != nullptr) {
        decoded = transfer_->finishInput();
    }
    if (decoded.complete() != nullptr) {
        transferPhase_ = TransferPhase::kEnded;
        return complete(consumed);
    }
    if (const auto* output = decoded.output()) {
        if (zeroContent_ && !output->bytes().empty()) {
            return fail(Http1ClientResponseBodyError::kNonEmpty205, consumed);
        }
        transferPhase_ = TransferPhase::kDrainOutput;
        return emit(consumed, output->bytes());
    }
    if (decoded.decoderFailure() != nullptr) {
        return decoderFailure(consumed);
    }
    if (decoded.failure() != nullptr) {
        return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
    }
    return fail(Http1ClientResponseBodyError::kIncompleteBody, consumed);
}

Http1ClientResponseBodyDecoder::Result
Http1ClientResponseBodyDecoder::replayTerminal() const noexcept {
    return std::visit([](const auto& payload) -> Result {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, Complete>) {
            return Result(Complete(0, payload.persistence()));
        } else if constexpr (std::is_same_v<T, ProtocolFailure>) {
            return Result(ProtocolFailure(0, payload.error()));
        } else {
            static_assert(std::is_same_v<T, DecoderFailure>);
            return Result(DecoderFailure(0));
        }
    },
        *terminal_);
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::needInput(
    std::size_t consumed) const noexcept {
    return Result(NeedInput(consumed));
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::emit(
    std::size_t consumed, std::string_view bytes) const noexcept {
    return Result(OutputView(consumed, bytes));
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::validatedTrailers(
    std::size_t consumed, std::string_view bytes) {
    return Result(ValidatedTrailersView(consumed, bytes));
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::complete(
    std::size_t consumed) {
    const auto completed = Complete(consumed, persistence_);
    terminal_ = completed;
    return Result(completed);
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::fail(
    Http1ClientResponseBodyError error, std::size_t consumed) {
    const auto failure = ProtocolFailure(consumed, error);
    terminal_ = failure;
    return Result(failure);
}

Http1ClientResponseBodyDecoder::Result Http1ClientResponseBodyDecoder::decoderFailure(
    std::size_t consumed) {
    const auto failure = DecoderFailure(consumed);
    terminal_ = failure;
    return Result(failure);
}

std::size_t Http1ClientResponseBodyDecoder::Result::consumedBytes() const noexcept {
    return std::visit([](const auto& payload) { return payload.consumedBytes(); }, value_);
}

const Http1ClientResponseBodyDecoder::NeedInput*
Http1ClientResponseBodyDecoder::Result::needInput() const& noexcept {
    return std::get_if<NeedInput>(&value_);
}

const Http1ClientResponseBodyDecoder::OutputView*
Http1ClientResponseBodyDecoder::Result::output() const& noexcept {
    return std::get_if<OutputView>(&value_);
}

const Http1ClientResponseBodyDecoder::ValidatedTrailersView*
Http1ClientResponseBodyDecoder::Result::trailers() const& noexcept {
    return std::get_if<ValidatedTrailersView>(&value_);
}

const Http1ClientResponseBodyDecoder::Complete*
Http1ClientResponseBodyDecoder::Result::complete() const& noexcept {
    return std::get_if<Complete>(&value_);
}

const Http1ClientResponseBodyDecoder::ProtocolFailure*
Http1ClientResponseBodyDecoder::Result::protocolFailure() const& noexcept {
    return std::get_if<ProtocolFailure>(&value_);
}

const Http1ClientResponseBodyDecoder::DecoderFailure*
Http1ClientResponseBodyDecoder::Result::decoderFailure() const& noexcept {
    return std::get_if<DecoderFailure>(&value_);
}

}  // namespace ruvia
