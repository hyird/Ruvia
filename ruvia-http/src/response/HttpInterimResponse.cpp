#include "ruvia/http/HttpInterimResponse.h"

#include "ruvia/http/HttpStatus.h"

#include <stdexcept>

namespace ruvia {

HttpInterimResponseHead::HttpInterimResponseHead(HttpStatusCode statusCode, HeaderInit headers)
    : statusCode_(statusCode),
      headers_(headers) {
    if (!detail::httpInterimStatusCodeValid(statusCode)) {
        throw std::invalid_argument(statusCode == http_status::kSwitchingProtocols
                                        ? "Switching Protocols requires a dedicated protocol driver"
                                        : "invalid interim HTTP status code");
    }
}

}  // namespace ruvia
