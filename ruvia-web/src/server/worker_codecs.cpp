#include "server/worker_codecs.h"

#include <exception>
#include <stdexcept>

#include "ruvia/core/failure_report.h"
#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {

worker_codecs::worker_codecs() noexcept
    : content_codecs_(process_resource()),
      websocket_deflate_(process_resource()) {}

worker_codecs::~worker_codecs() = default;

}  // namespace ruvia::detail
