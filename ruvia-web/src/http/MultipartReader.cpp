#include "ruvia/http/MultipartReader.h"

#include "ruvia/app/Task.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

Task<std::optional<MultipartStreamPart>> MultipartReader::read() {
    for (;;) {
        auto result = parser_.poll();
        switch (result.status) {
            case MultipartParser::PollStatus::kPart:
                co_return std::move(result.part);
            case MultipartParser::PollStatus::kDone:
                co_return std::nullopt;
            case MultipartParser::PollStatus::kNeedMore: {
                auto chunk = co_await bodyReader_.read();
                if (!chunk) {
                    throw std::invalid_argument("invalid multipart body");
                }
                parser_.appendChunk(*chunk);
                break;
            }
            case MultipartParser::PollStatus::kContinue:
                break;
        }
    }
}

}  // namespace ruvia
