#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include <asio.hpp>
#include "ruvia/web/detail/body/HttpContinueWriter.h"
#include "ruvia/core/detail/AsioAwait.h"

#include "ruvia/web/detail/body/HttpStreamBodyReaderCore.inl"
#include "ruvia/web/detail/body/HttpStreamBodyReaderPipeline.inl"
#include "ruvia/web/detail/body/HttpStreamBodyReaderContentLength.inl"
#include "ruvia/web/detail/body/HttpStreamBodyReaderChunked.inl"
