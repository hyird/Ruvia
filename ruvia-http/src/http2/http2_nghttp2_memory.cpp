#include "http2/http2_nghttp2_memory.h"

namespace ruvia::detail {

http2_nghttp2_memory::http2_nghttp2_memory(std::pmr::memory_resource* resource) noexcept
    : memory_(resource),
      table_{.mem_user_data = &memory_,
          .malloc = &http_c_library_memory::allocate,
          .free = &http_c_library_memory::deallocate,
          .calloc = &http_c_library_memory::allocate_zeroed,
          .realloc = &http_c_library_memory::reallocate} {}

}  // namespace ruvia::detail
