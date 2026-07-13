#include "ruvia/web/MultipartReader.h"

#include "ruvia/core/Task.h"
#include "ruvia/http/HttpProtocolError.h"

#include <stdexcept>
#include <utility>

namespace ruvia {

Task<std::optional<MultipartStreamPart>> MultipartReader::read() {
    for (;;) {
        auto result = parser_.poll();
        if (const auto* part = result.part()) {
            co_return *part;
        }
        if (result.done() != nullptr) {
            // RFC 2046 permits an epilogue after the closing delimiter. It is
            // semantically ignored but the HTTP body still has to be consumed
            // before the connection can be reused.
            while (!bodyEnded_ && co_await bodyReader_.read()) {}
            bodyEnded_ = true;
            co_return std::nullopt;
        }
        if (result.needInput() != nullptr) {
            auto chunk = co_await bodyReader_.read();
            if (!chunk) {
                bodyEnded_ = true;
                parser_.finishInput();
            } else {
                parser_.feed(*chunk);
            }
            continue;
        }
        if (const auto* failure = result.failure()) {
            throw HttpProtocolError(
                400, multipartParseErrorMessage(failure->error()));
        }
        throw std::logic_error("unexpected multipart poll result");
    }
}

}  // namespace ruvia
