#pragma once

#include "ruvia/http/Streaming.h"

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

namespace ruvia {

struct MultipartStreamPart {
    std::string_view name;
    std::string_view filename;
    std::string_view contentType;
    std::string_view body;
    bool partBegin{false};
    bool partEnd{false};
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
};

}  // namespace ruvia
