#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include <asio.hpp>

#include "ruvia/core/async.h"

#include "body/http_continue_writer.h"
#include "body/http_stream_body_reader_chunked.inl"
#include "body/http_stream_body_reader_content_length.inl"
#include "body/http_stream_body_reader_core.inl"
#include "body/http_stream_body_reader_pipeline.inl"
