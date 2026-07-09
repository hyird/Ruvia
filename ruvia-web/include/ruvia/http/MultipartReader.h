#pragma once

#include "ruvia/app/Task.h"
#include "ruvia/http/MultipartParser.h"
#include "ruvia/http/Streaming.h"

#include <memory_resource>
#include <optional>
#include <string_view>

namespace ruvia {

class MultipartReader final {
public:
    MultipartReader(BodyReader& bodyReader, std::string_view boundary, std::pmr::memory_resource* resource)
        : bodyReader_(bodyReader),
          parser_(boundary, resource) {}

    [[nodiscard]] Task<std::optional<MultipartStreamPart>> read();

private:
    BodyReader& bodyReader_;
    MultipartParser parser_;
};

}  // namespace ruvia
