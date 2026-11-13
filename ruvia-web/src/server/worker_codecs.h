#pragma once

#include <array>
#include <cstddef>
#include <memory_resource>
#include <utility>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/http/http_content_codec.h"
#include "ruvia/http/http_content_coding.h"
#include "ruvia/http/http_content_encoder.h"
#include "ruvia/http/websocket_deflate_codecs.h"

namespace ruvia::detail {

using stream_encoder_lease = http_stream_encoder_lease;

class worker_codecs final {
public:
    worker_codecs() noexcept;
    ~worker_codecs();
    worker_codecs(const worker_codecs&) = delete;
    worker_codecs& operator=(const worker_codecs&) = delete;

    [[nodiscard]] http_content_encode_context& encoder(http_content_coding coding) {
        return content_codecs_.encoder(coding);
    }

    [[nodiscard]] stream_encoder_lease acquire_stream_encoder(http_content_coding coding) {
        return content_codecs_.acquire_stream_encoder(coding);
    }

    [[nodiscard]] websocket_deflate_codecs& websocket_deflate() noexcept {
        return websocket_deflate_;
    }

private:
    http_worker_codecs content_codecs_;
    websocket_deflate_codecs websocket_deflate_;
};
}  // namespace ruvia::detail
