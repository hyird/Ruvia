#pragma once

#include "ruvia/http/http3_connection_error.h"
#include "ruvia/http/http3_message_head.h"

namespace ruvia::detail {

// The enclosing request/response or PUSH_PROMISE chooses the error scope.
[[nodiscard]] inline http3_connection_error_code http3_message_head_error_code(http3_message_head_error error) noexcept {
    switch (error) {
        case http3_message_head_error::message_error:
            return http3_connection_error_code::message_error;
        case http3_message_head_error::qpack_decompression_failed:
            return http3_connection_error_code::qpack_decompression_failed;
        case http3_message_head_error::field_section_too_large:
            return http3_connection_error_code::excessive_load;
    }
    return http3_connection_error_code::message_error;
}

}  // namespace ruvia::detail
