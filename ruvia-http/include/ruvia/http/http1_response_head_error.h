#pragma once

#include <stdexcept>

namespace ruvia {

// An HTTP/1 response head this endpoint cannot send: an interim, final,
// CONNECT or CONNECT-UDP head that breaks a message rule (RFC 9110, RFC 9112)
// or the head size bound. It reports an application or runtime bug, never a
// peer error, so a server answers it like any other handler failure.
class http1_response_head_error final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

}  // namespace ruvia
