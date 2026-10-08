#pragma once

#include <vector>

#include "ruvia/http/HttpHeader.h"
#include "ruvia/web/HttpClientUploadConfig.h"

#include "client/HttpClientOutputQueue.h"

namespace ruvia::detail {
class HttpClientUploadState final {
public:
    HttpClientUploadState(const WorkerHandle& worker, std::pmr::memory_resource* resource, HttpClientUploadConfig config)
        : output(worker, resource),
          config(config),
          trailers(resource) {}
    HttpClientUploadState(WorkerHandle&&, std::pmr::memory_resource*, HttpClientUploadConfig) = delete;
    http_client_output_queue output;
    HttpClientUploadConfig config;
    std::pmr::vector<HttpHeader> trailers;
    bool contentReleased{};
    bool responseTaken{};
    ::ruvia::operation_scope responseScope;
};
}  // namespace ruvia::detail
