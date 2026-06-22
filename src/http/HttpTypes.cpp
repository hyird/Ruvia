#include "ruvia/http/HttpCommon.h"

namespace ruvia {

MultipartPart::MultipartPart(std::pmr::memory_resource* resource)
    : name(resource), filename(resource), contentType(resource) {}

}  // namespace ruvia
