#include "server/worker_codecs.h"

#include <exception>
#include <stdexcept>

#include "ruvia/core/failure_report.h"

namespace ruvia::detail {

worker_codecs::worker_codecs() noexcept
    : content_codecs_(),
      websocket_deflate_() {}

worker_codecs::~worker_codecs() = default;

}  // namespace ruvia::detail
