#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/HttpProtocolError.h"

namespace ruvia {

namespace detail {
struct MultipartPartAccess;
struct MultipartStreamPartAccess;
}  // namespace detail

// Buffered multipart parsing owns decoded Content-Disposition names while the
// content type and body remain borrowed views into the caller-owned request body.
class MultipartPart final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return std::string_view(name_.data(), name_.size());
    }

    [[nodiscard]] std::string_view filename() const noexcept {
        return std::string_view(filename_.data(), filename_.size());
    }

    [[nodiscard]] std::string_view contentType() const noexcept {
        return contentType_;
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

private:
    friend struct detail::MultipartPartAccess;

    MultipartPart(
        std::pmr::string name,
        std::pmr::string filename,
        std::string_view contentType,
        std::string_view body) noexcept
        : name_(std::move(name)),
          filename_(std::move(filename)),
          contentType_(contentType),
          body_(body) {}

    std::pmr::string name_;
    std::pmr::string filename_;
    std::string_view contentType_;
    std::string_view body_;
};

// RFC 2046 multipart boundary value. The validated bytes are stored inline so
// every buffered/streaming parser consumes the same allocation-free value and
// cannot observe a dangling Content-Type substring.
class MultipartBoundary final {
public:
    explicit MultipartBoundary(std::string_view value) {
        if (!valid(value)) {
            throw std::invalid_argument("invalid multipart boundary");
        }
        assign(value);
    }

    [[nodiscard]] constexpr std::string_view value() const noexcept {
        return std::string_view(bytes_.data(), size_);
    }

private:
    static constexpr std::size_t kMaxSize = 70;

    [[nodiscard]] static constexpr bool nonSpaceChar(char value) noexcept {
        return (value >= '0' && value <= '9') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            value == '\'' || value == '(' || value == ')' || value == '+' ||
            value == '_' || value == ',' || value == '-' || value == '.' ||
            value == '/' || value == ':' || value == '=' || value == '?';
    }

    [[nodiscard]] static constexpr bool valid(std::string_view value) noexcept {
        if (value.empty() || value.size() > kMaxSize || value.back() == ' ') {
            return false;
        }
        for (const char byte : value) {
            if (byte != ' ' && !nonSpaceChar(byte)) {
                return false;
            }
        }
        return true;
    }

    constexpr void assign(std::string_view value) noexcept {
        for (std::size_t index = 0; index < value.size(); ++index) {
            bytes_[index] = value[index];
        }
        size_ = static_cast<std::uint8_t>(value.size());
    }

    std::array<char, kMaxSize> bytes_{};
    std::uint8_t size_{0};
};

enum class MultipartChunkPhase : std::uint8_t {
    kComplete,
    kFirst,
    kMiddle,
    kLast,
};

class MultipartStreamPart final {
public:
    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }

    [[nodiscard]] std::string_view filename() const noexcept {
        return filename_;
    }

    [[nodiscard]] std::string_view contentType() const noexcept {
        return contentType_;
    }

    [[nodiscard]] std::string_view body() const noexcept {
        return body_;
    }

    [[nodiscard]] constexpr MultipartChunkPhase phase() const noexcept {
        return phase_;
    }

private:
    friend struct detail::MultipartStreamPartAccess;

    constexpr MultipartStreamPart(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType,
        std::string_view body,
        MultipartChunkPhase phase) noexcept
        : name_(name),
          filename_(filename),
          contentType_(contentType),
          body_(body),
          phase_(phase) {}

    std::string_view name_;
    std::string_view filename_;
    std::string_view contentType_;
    std::string_view body_;
    MultipartChunkPhase phase_{MultipartChunkPhase::kMiddle};
};

class MultipartPollNeedInput final {
private:
    friend class MultipartPollResult;
    constexpr MultipartPollNeedInput() noexcept = default;
};

class MultipartPollDone final {
private:
    friend class MultipartPollResult;
    constexpr MultipartPollDone() noexcept = default;
};

enum class MultipartParseError : std::uint8_t {
    kIncompleteBody,
    kInvalidDelimiter,
    kPreambleTooLarge,
    kPartHeadersTooLarge,
    kInvalidContentDisposition,
    kMissingFieldName,
    kDelimiterLineTooLarge,
};

class MultipartPollFailure final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept;

private:
    friend class MultipartPollResult;
    friend class MultipartBodyParseResult;

    explicit constexpr MultipartPollFailure(MultipartParseError error) noexcept
        : error_(error) {}

    MultipartParseError error_;
};

// Incremental multipart parsing has four mutually exclusive outcomes. Only
// the part alternative exposes borrowed part metadata/body, while only a
// failure exposes a protocol error. Need-input and done are payload-free
// lifecycle signals. All borrowed views remain valid only until the next
// feed(), finishInput(), or poll() call.
class MultipartPollResult final {
public:
    [[nodiscard]] constexpr const MultipartPollNeedInput*
    needInput() const & noexcept {
        return std::get_if<MultipartPollNeedInput>(&value_);
    }
    const MultipartPollNeedInput* needInput() const && = delete;

    [[nodiscard]] constexpr const MultipartStreamPart* part() const & noexcept {
        return std::get_if<MultipartStreamPart>(&value_);
    }
    const MultipartStreamPart* part() const && = delete;

    [[nodiscard]] constexpr const MultipartPollDone* done() const & noexcept {
        return std::get_if<MultipartPollDone>(&value_);
    }
    const MultipartPollDone* done() const && = delete;

    [[nodiscard]] constexpr const MultipartPollFailure*
    failure() const & noexcept {
        return std::get_if<MultipartPollFailure>(&value_);
    }
    const MultipartPollFailure* failure() const && = delete;

private:
    friend class MultipartParser;

    using Value = std::variant<
        MultipartPollNeedInput,
        MultipartStreamPart,
        MultipartPollDone,
        MultipartPollFailure>;

    explicit constexpr MultipartPollResult(MultipartPollNeedInput value) noexcept
        : value_(value) {}

    explicit constexpr MultipartPollResult(MultipartStreamPart value) noexcept
        : value_(value) {}

    explicit constexpr MultipartPollResult(MultipartPollDone value) noexcept
        : value_(value) {}

    explicit constexpr MultipartPollResult(MultipartPollFailure value) noexcept
        : value_(value) {}

    [[nodiscard]] static constexpr MultipartPollResult makeNeedInput() noexcept {
        return MultipartPollResult(MultipartPollNeedInput());
    }

    [[nodiscard]] static constexpr MultipartPollResult makePart(
        MultipartStreamPart part) noexcept {
        return MultipartPollResult(part);
    }

    [[nodiscard]] static constexpr MultipartPollResult makeDone() noexcept {
        return MultipartPollResult(MultipartPollDone());
    }

    [[nodiscard]] static constexpr MultipartPollResult makeFailure(
        MultipartParseError error) noexcept {
        return MultipartPollResult(MultipartPollFailure(error));
    }

    Value value_;
};

class MultipartBody final {
public:
    MultipartBody(const MultipartBody&) = delete;
    MultipartBody& operator=(const MultipartBody&) = delete;
    MultipartBody(MultipartBody&&) noexcept = default;
    MultipartBody& operator=(MultipartBody&&) = delete;

    [[nodiscard]] const std::pmr::vector<MultipartPart>& parts() const & noexcept {
        return parts_;
    }
    const std::pmr::vector<MultipartPart>& parts() const && = delete;

    [[nodiscard]] std::pmr::vector<MultipartPart> takeParts() && noexcept {
        return std::move(parts_);
    }

private:
    friend class MultipartBodyParseResult;

    explicit MultipartBody(std::pmr::vector<MultipartPart> parts) noexcept
        : parts_(std::move(parts)) {}

    std::pmr::vector<MultipartPart> parts_;
};

class MultipartBodyParseFailure final {
public:
    [[nodiscard]] HttpProtocolError protocolError() const noexcept;

private:
    friend class MultipartBodyParseResult;

    explicit constexpr MultipartBodyParseFailure(
        MultipartParseError error) noexcept
        : error_(error) {}

    MultipartParseError error_;
};

class MultipartBodyParseResult final {
public:
    MultipartBodyParseResult(const MultipartBodyParseResult&) = delete;
    MultipartBodyParseResult& operator=(const MultipartBodyParseResult&) = delete;
    MultipartBodyParseResult(MultipartBodyParseResult&&) noexcept = default;
    MultipartBodyParseResult& operator=(MultipartBodyParseResult&&) = delete;

    [[nodiscard]] MultipartBody* body() & noexcept {
        return std::get_if<MultipartBody>(&value_);
    }

    [[nodiscard]] const MultipartBody* body() const & noexcept {
        return std::get_if<MultipartBody>(&value_);
    }
    MultipartBody* body() && = delete;
    const MultipartBody* body() const && = delete;

    [[nodiscard]] const MultipartBodyParseFailure* failure() const & noexcept {
        return std::get_if<MultipartBodyParseFailure>(&value_);
    }
    const MultipartBodyParseFailure* failure() const && = delete;

private:
    friend MultipartBodyParseResult parseMultipartBody(
        std::string_view,
        MultipartBoundary,
        std::pmr::memory_resource*);

    using Value = std::variant<MultipartBody, MultipartBodyParseFailure>;

    explicit MultipartBodyParseResult(std::pmr::vector<MultipartPart> parts) noexcept
        : value_(MultipartBody(std::move(parts))) {}

    explicit MultipartBodyParseResult(
        MultipartParseError error) noexcept
        : value_(MultipartBodyParseFailure(error)) {}

    explicit MultipartBodyParseResult(
        const MultipartPollFailure& failure) noexcept
        : value_(MultipartBodyParseFailure(failure.error_)) {}

    Value value_;
};

class MultipartParser final {
public:
    MultipartParser(MultipartBoundary boundary, std::pmr::memory_resource* resource);

    MultipartParser(const MultipartParser&) = delete;
    MultipartParser& operator=(const MultipartParser&) = delete;
    MultipartParser(MultipartParser&&) = delete;
    MultipartParser& operator=(MultipartParser&&) = delete;

    // Copies input into parser-owned PMR storage. finishInput() is required when
    // the enclosing HTTP body ends so a close delimiter ending exactly at EOF
    // can be distinguished from a delimiter line split across input chunks.
    // A protocol failure is terminal: later poll() calls repeat the exact error
    // and feed() rejects further bytes.
    void feed(std::string_view chunk);
    void finishInput() noexcept;

    [[nodiscard]] MultipartPollResult poll();

private:
    struct CompleteInputTag final {};

    friend MultipartBodyParseResult parseMultipartBody(
        std::string_view,
        MultipartBoundary,
        std::pmr::memory_resource*);

    MultipartParser(
        std::string_view completeBody,
        MultipartBoundary boundary,
        std::pmr::memory_resource* resource,
        CompleteInputTag);

    enum class ProgressState : std::uint8_t {
        kBoundary,
        kHeaders,
        kBody,
        kDone
    };

    using State = std::variant<ProgressState, MultipartParseError>;

    enum class StepProgress : std::uint8_t {
        kNeedInput,
        kContinue,
        kDone,
    };

    using StepResult = std::variant<StepProgress, MultipartParseError>;

    static constexpr std::size_t kCompactConsumedPrefixBytes = 64 * 1024;

    [[nodiscard]] std::string_view bufferView() const noexcept;
    void consume(std::size_t bytes) noexcept;
    void compactConsumedPrefix();
    void compactPending();
    [[nodiscard]] MultipartPollResult fail(
        MultipartParseError error) noexcept;
    [[nodiscard]] StepResult processBoundary();
    [[nodiscard]] StepResult processHeaders();
    [[nodiscard]] MultipartStreamPart makePart(std::string_view body, bool partEnd);
    [[nodiscard]] MultipartPollResult readBodyChunk();

    std::pmr::memory_resource* resource_;
    MultipartBoundary boundary_;
    std::pmr::string buffer_;
    std::pmr::string currentName_;
    std::pmr::string currentFilename_;
    std::pmr::string currentContentType_;
    std::string_view currentContentTypeView_;
    std::string_view borrowedInput_;
    bool borrowedInputMode_{false};
    State state_{ProgressState::kBoundary};
    std::size_t bufferOffset_{0};
    std::size_t pendingEraseBytes_{0};
    bool nextChunkIsFirst_{false};
    bool firstBoundary_{true};
    bool inputFinished_{false};
};

// Parses a complete multipart/form-data body without I/O. Returned part bodies
// and content types borrow `body`; decoded name/filename values own PMR storage.
[[nodiscard]] MultipartBodyParseResult parseMultipartBody(
    std::string_view body,
    MultipartBoundary boundary,
    std::pmr::memory_resource* resource = nullptr);

}  // namespace ruvia
