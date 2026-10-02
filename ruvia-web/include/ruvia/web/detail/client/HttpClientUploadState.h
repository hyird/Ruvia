#pragma once

#include <vector>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/HttpClientUploadConfig.h"
#include "ruvia/web/detail/client/HttpClientOutputQueue.h"

namespace ruvia::detail {
class HttpClientUploadState final : public HttpClientOutputQueue {
public:
    HttpClientUploadState(const WorkerHandle& worker, std::pmr::memory_resource* resource, HttpClientUploadConfig config)
        : HttpClientOutputQueue(worker, resource),
          config(config),
          trailers(resource) {}
    HttpClientUploadConfig config;
    std::pmr::vector<HttpHeader> trailers;
    bool contentReleased{};
    bool responseTaken{};
    ScopedOperationScope responseScope;
};
}  // namespace ruvia::detail
