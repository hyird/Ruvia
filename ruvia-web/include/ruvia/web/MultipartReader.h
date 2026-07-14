#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/http/MultipartParser.h"
#include "ruvia/web/Streaming.h"

#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>

namespace ruvia {

class MultipartReader final {
public:
    MultipartReader(
        BodyReader& bodyReader,
        MultipartBoundary boundary,
        std::pmr::memory_resource* resource)
        : bodyReader_(bodyReader),
          parser_(std::move(boundary), resource) {}

    MultipartReader(const MultipartReader&) = delete;
    MultipartReader& operator=(const MultipartReader&) = delete;
    MultipartReader(MultipartReader&&) = delete;
    MultipartReader& operator=(MultipartReader&&) = delete;

    /// Returns one typed chunk of the current multipart part. All views in the
    /// returned value remain valid only until the next read() call.
    [[nodiscard]] Task<std::optional<MultipartStreamPart>> read();

private:
    BodyReader& bodyReader_;
    MultipartParser parser_;
    bool bodyEnded_{false};
};

}  // namespace ruvia
