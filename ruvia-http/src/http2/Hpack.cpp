#include "ruvia/http/Hpack.h"

#include "ruvia/http/detail/http2/hpack/Http2Hpack.h"

namespace ruvia {

class HpackDecoder::Impl final {
public:
    explicit Impl(std::pmr::memory_resource* resource)
        : decoder({.resource = resource}) {}
    detail::HpackDecoder decoder;
};

HpackDecoder::HpackDecoder(HpackDecoderOptions options)
    : impl_(std::make_unique<Impl>(options.resource)) {}
HpackDecoder::~HpackDecoder() = default;
HpackDecoder::HpackDecoder(HpackDecoder&&) noexcept = default;
HpackDecoder& HpackDecoder::operator=(HpackDecoder&&) noexcept = default;

void HpackDecoder::setMaxDynamicTableSize(std::size_t bytes) {
    impl_->decoder.setMaxDynamicTableSize(bytes);
}

HpackDecodeResult HpackDecoder::decodeWithCallback(std::string_view block, void* target, HeaderCallback callback) {
    const auto result = impl_->decoder.decode(block, target, callback);
    const auto* failure = result.failure();
    return HpackDecodeResult(failure == nullptr ? std::nullopt : std::optional<HpackDecodeError>(static_cast<HpackDecodeError>(failure->error())));
}

void HpackEncoder::encodeIndexed(std::pmr::string& output, std::uint32_t index) {
    detail::HpackEncoder::encodeIndexed(output, index);
}
void HpackEncoder::encodeDynamicTableSizeUpdate(std::pmr::string& output, std::uint32_t maximum) {
    detail::HpackEncoder::encodeDynamicTableSizeUpdate(output, maximum);
}
void HpackEncoder::encodeHeader(std::pmr::string& output, std::string_view name, std::string_view value) {
    detail::HpackEncoder::encodeHeader(output, name, value);
}
void HpackEncoder::encodeHeaderWithNameIndex(std::pmr::string& output, std::uint32_t nameIndex, std::string_view value, HpackHeaderWithNameIndexOptions options) {
    detail::HpackEncoder::encodeHeaderWithNameIndex(output, nameIndex, value, options.neverIndexed);
}
void HpackEncoder::encodeStatus(std::pmr::string& output, HttpStatusCode status) {
    detail::HpackEncoder::encodeStatus(output, status);
}

}  // namespace ruvia
