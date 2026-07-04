#pragma once

#ifdef RUVIA_ENABLE_HTTP_CLIENT

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <vector>

#include "ruvia/app/Task.h"
#include "ruvia/http/HttpClient.h"

namespace ruvia::detail {

// Backend-specific producer of a streamed response body. The public FetchResponseStream is a
// thin pimpl over this. readChunk() yields the next slice of the (already content-decoded at the
// transport layer) body and an empty string at end of stream.
class FetchStreamSource {
public:
    virtual ~FetchStreamSource() = default;

    [[nodiscard]] virtual std::uint16_t status() const noexcept = 0;
    [[nodiscard]] virtual const std::pmr::vector<FetchResponseHeader>& headers() const noexcept = 0;
    [[nodiscard]] virtual Task<std::pmr::string> readChunk() = 0;
    virtual void close() noexcept = 0;

    // Destroy + deallocate self through the concrete type's PMR resource.
    virtual void destroy() noexcept = 0;
};

}  // namespace ruvia::detail

#endif  // RUVIA_ENABLE_HTTP_CLIENT
