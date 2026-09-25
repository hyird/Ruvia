#pragma once

// Public request-body decoder surface for runtime drivers. The underlying
// implementations remain protocol-owned; callers only supply/refill buffers.
#include "ruvia/http/detail/coding/HttpTransferCodingDecoder.h"
#include "ruvia/http/detail/http1/Http1ChunkedBodyDecoder.h"

namespace ruvia {

using detail::Http1ChunkDecodeBodyChunk;
using detail::Http1ChunkDecodeComplete;
using detail::Http1ChunkDecodeError;
using detail::Http1ChunkDecodeFailure;
using detail::Http1ChunkDecodeNeedMore;
using detail::Http1ChunkDecodeResult;
using detail::Http1ChunkedBodyDecoder;
using detail::Http1ChunkTrailerRole;
using detail::TransferCodingDecodeComplete;
using detail::TransferCodingDecodeError;
using detail::TransferCodingDecodeNeedInput;
using detail::TransferCodingDecodeOutput;
using detail::TransferCodingDecodeProtocolFailure;
using detail::TransferCodingDecoder;
using detail::TransferCodingDecodeResult;
using detail::TransferCodingDecoderFailure;

}  // namespace ruvia
