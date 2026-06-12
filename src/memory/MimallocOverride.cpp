#include <mimalloc.h>

#if defined(_WIN32)
#include <mimalloc-new-delete.h>
#endif

namespace ruvia::detail {

void ensureMimallocGlobalOverrideLinked() noexcept {
    // Keep this TU linked so Windows imports mimalloc early and owns global new/delete.
    (void)mi_version();
}

}  // namespace ruvia::detail
