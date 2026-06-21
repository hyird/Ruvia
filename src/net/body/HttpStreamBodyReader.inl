#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include <asio.hpp>
#include "HttpContinueWriter.h"
#include "../../runtime/AsioAwait.h"
#include "ruvia/http/Error.h"

#include "HttpStreamBodyReaderCore.inl"
#include "HttpStreamBodyReaderPipeline.inl"
#include "HttpStreamBodyReaderContentLength.inl"
#include "HttpStreamBodyReaderChunked.inl"
