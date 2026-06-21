#pragma once

#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Http2ResponseHeaders.h"
#include "Http2StreamState.h"
#include "../server/HttpResponseStreamHead.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/Context.h"
#include "ruvia/http/HttpTypes.h"

namespace ruvia::detail {

template <typename Session>
class Http2ResponseStreamSink final {
public:
    Http2ResponseStreamSink(
        Session& session,
        Http2StreamState& stream,
        ResponseBodyMode mode) noexcept
        : session_(session),
          stream_(stream),
          mode_(mode),
          scratch_(session.memory_.resource()) {}

    [[nodiscard]] bool committed() const noexcept {
        return committed_;
    }

    static Task<void> writeThunk(void* target, std::string_view chunk) {
        co_await static_cast<Http2ResponseStreamSink*>(target)->write(chunk);
    }

    static Task<void> endThunk(void* target) {
        co_await static_cast<Http2ResponseStreamSink*>(target)->end();
    }

    static void bindContextThunk(void* target, Context* context) noexcept {
        static_cast<Http2ResponseStreamSink*>(target)->context_ = context;
    }

    static std::pmr::string& scratchThunk(void* target) noexcept {
        auto* self = static_cast<Http2ResponseStreamSink*>(target);
        self->scratch_.clear();
        return self->scratch_;
    }

private:
    Task<void> commit() {
        if (committed_) {
            co_return;
        }
        if (ended_) {
            throw std::logic_error("response stream is already ended");
        }
        if (context_ == nullptr) {
            throw std::logic_error("response stream context is not bound");
        }

        auto streamHead = prepareResponseStreamHead(*context_, mode_, ResponseStreamFraming::kHttp2DataFrames);
        bodyForbidden_ = streamHead.bodyForbidden;
        appendHttp2ResponseHeaders(stream_, streamHead.response, 0, false);
        committed_ = true;
        co_await session_.writeHeaders(stream_, stream_.responseHeaderBlock, bodyForbidden_);
        if (bodyForbidden_) {
            ended_ = true;
        }
    }

    Task<void> write(std::string_view chunk) {
        if (chunk.empty()) {
            co_return;
        }
        co_await commit();
        if (bodyForbidden_) {
            throw std::logic_error("response status does not allow a stream body");
        }
        co_await session_.writeData(stream_, chunk, {}, false);
    }

    Task<void> end() {
        if (ended_) {
            co_return;
        }
        co_await commit();
        if (bodyForbidden_) {
            ended_ = true;
            co_return;
        }
        co_await session_.writeData(stream_, {}, {}, true);
        ended_ = true;
    }

    Session& session_;
    Http2StreamState& stream_;
    ResponseBodyMode mode_;
    Context* context_{nullptr};
    std::pmr::string scratch_;
    bool committed_{false};
    bool ended_{false};
    bool bodyForbidden_{false};
};

}  // namespace ruvia::detail
