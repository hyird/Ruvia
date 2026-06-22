#pragma once

#include "../../http/HttpResponseFileBody.h"
#include "ruvia/app/Task.h"
#include "ruvia/http/HttpTypes.h"

#include <asio/ip/tcp.hpp>
#include <system_error>

namespace ruvia::detail {

Task<void> writeFileZeroCopy(asio::ip::tcp::socket& socket, ResponseFileBody file, std::error_code& ec);

}  // namespace ruvia::detail
