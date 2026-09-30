#include "ruvia/http/Hpack.h"

#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"
#include "ruvia/http/detail/util/HttpPmrObject.h"
#include "ruvia/http/detail/util/PmrResource.h"

namespace ruvia {

class HpackDecoder::Impl final {
public:
    explicit Impl(std::pmr::memory_resource* memory)
        : resource_(memory),
          decoder_({.resource = memory}) {}

    std::pmr::memory_resource* resource_;
    detail::HpackDecoder decoder_;
};

void HpackDecoder::ImplDeleter::operator()(Impl* value) const noexcept {
    if (value != nullptr) {
        detail::destroyHttpPmrObject(value, value->resource_);
    }
}

HpackDecoder::HpackDecoder(HpackDecoderOptions options) {
    auto* resource = detail::httpPmrResourceOrDefault(options.resource);
    impl_.reset(detail::constructHttpPmrObject<Impl>(resource, resource));
}
HpackDecoder::~HpackDecoder() = default;
HpackDecoder::HpackDecoder(HpackDecoder&&) noexcept = default;
HpackDecoder& HpackDecoder::operator=(HpackDecoder&&) noexcept = default;

void HpackDecoder::setMaxDynamicTableSize(std::size_t bytes) {
    impl_->decoder_.setMaxDynamicTableSize(bytes);
}

HpackDecodeResult HpackDecoder::decodeWithCallback(
    std::string_view block, void* target, HeaderCallback callback) {
    return impl_->decoder_.decode(block, target, callback);
}

void HpackEncoder::encodeIndexed(std::pmr::string& output, std::uint32_t index) {
    detail::HpackEncoder::encodeIndexed(output, index);
}
void HpackEncoder::encodeDynamicTableSizeUpdate(std::pmr::string& output, std::uint32_t maximum) {
    detail::HpackEncoder::encodeDynamicTableSizeUpdate(output, maximum);
}
void HpackEncoder::encodeHeader(
    std::pmr::string& output, std::string_view name, std::string_view value) {
    detail::HpackEncoder::encodeHeader(output, name, value);
}
void HpackEncoder::encodeHeaderWithNameIndex(std::pmr::string& output, std::uint32_t nameIndex,
    std::string_view value, HpackHeaderWithNameIndexOptions options) {
    detail::HpackEncoder::encodeHeaderWithNameIndex(output, nameIndex, value, options.neverIndexed);
}
void HpackEncoder::encodeStatus(std::pmr::string& output, HttpStatusCode status) {
    detail::HpackEncoder::encodeStatus(output, status);
}

}  // namespace ruvia
