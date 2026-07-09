#include "ruvia/http/MultipartReader.h"

#include "ruvia/app/Task.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

Task<std::optional<MultipartStreamPart>> MultipartReader::read() {
    for (;;) {
        auto result = poll();
        switch (result.status) {
            case PollStatus::kPart:
                co_return std::move(result.part);
            case PollStatus::kDone:
                co_return std::nullopt;
            case PollStatus::kNeedMore: {
                auto chunk = co_await bodyReader_.read();
                if (!chunk) {
                    throw std::invalid_argument("invalid multipart body");
                }
                appendChunk(*chunk);
                break;
            }
            case PollStatus::kContinue:
                break;
        }
    }
}

}  // namespace ruvia
