#include "ruvia/http/http_interim_response.h"

#include <stdexcept>

#include "ruvia/http/http_status.h"

namespace ruvia {

http_interim_response_head::http_interim_response_head(http_status_code status_code, header_init_type headers)
    : status_code_(status_code),
      headers_(headers) {
    if (!detail::http_interim_status_code_valid(status_code)) {
        throw std::invalid_argument(status_code == http_status::switching_protocols
                                        ? "Switching Protocols requires a dedicated protocol driver"
                                        : "invalid interim HTTP status code");
    }
}

}  // namespace ruvia
