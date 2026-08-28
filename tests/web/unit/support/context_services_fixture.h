#pragma once

#include "test_io_context.h"

#include <ruvia/core/EventLoopAttachment.h>
#include <ruvia/web/detail/http/context/ContextServices.h>

namespace ruvia::test {

// Pure Context tests still need the same mandatory, address-stable worker
// borrow as production dispatch. The attached endpoint is intentionally not
// driven: tests that perform worker I/O own and run their local EventLoop.
[[nodiscard]] inline const WorkerHandle& testWorkerHandle() {
    static auto attachment = attachEventLoop(newTestIoContext());
    static const auto worker = attachment.loop().handle();
    return worker;
}

[[nodiscard]] inline detail::ContextServices testContextServices() {
    return detail::ContextServices(testWorkerHandle());
}

}  // namespace ruvia::test
