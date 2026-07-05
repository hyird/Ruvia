#pragma once

#include "ruvia/http/Streaming.h"

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia {

namespace detail {
struct MultipartStreamPartAccess;
}  // namespace detail

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

    [[nodiscard]] bool partBegin() const noexcept {
        return partBegin_;
    }

    [[nodiscard]] bool partEnd() const noexcept {
        return partEnd_;
    }

private:
    friend struct detail::MultipartStreamPartAccess;

    constexpr MultipartStreamPart(
        std::string_view name,
        std::string_view filename,
        std::string_view contentType,
        std::string_view body,
        bool partBegin,
        bool partEnd) noexcept
        : name_(name),
          filename_(filename),
          contentType_(contentType),
          body_(body),
          partBegin_(partBegin),
          partEnd_(partEnd) {}

    std::string_view name_;
    std::string_view filename_;
    std::string_view contentType_;
    std::string_view body_;
    bool partBegin_{false};
    bool partEnd_{false};
};

class MultipartReader final {
public:
    MultipartReader(BodyReader& bodyReader, std::string_view boundary, std::pmr::memory_resource* resource);

    [[nodiscard]] Task<std::optional<MultipartStreamPart>> read();

private:
    enum class State {
        kBoundary,
        kHeaders,
        kBody,
        kDone
    };

    static constexpr std::size_t kCompactConsumedPrefixBytes = 64 * 1024;

    [[nodiscard]] std::string_view bufferView() const noexcept;
    void consume(std::size_t bytes) noexcept;
    void compactConsumedPrefix();
    void compactPending();
    Task<bool> appendMore();
    Task<void> processBoundary();
    Task<void> processHeaders();
    [[nodiscard]] MultipartStreamPart makePart(std::string_view body, bool partEnd);
    Task<std::optional<MultipartStreamPart>> readBodyChunk();

    BodyReader& bodyReader_;
    std::pmr::memory_resource* resource_;
    std::pmr::string buffer_;
    std::pmr::string boundaryLine_;
    std::pmr::string boundaryPrefix_;
    std::pmr::string currentName_;
    std::pmr::string currentFilename_;
    std::pmr::string currentContentType_;
    State state_{State::kBoundary};
    std::size_t bufferOffset_{0};
    std::size_t pendingEraseBytes_{0};
    bool partBegin_{false};
    bool firstBoundary_{true};
};

}  // namespace ruvia
