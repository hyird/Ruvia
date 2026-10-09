#include "ruvia/http/hpack.h"

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"

#include "http2/http2_hpack.h"

namespace ruvia {

class hpack_decoder::impl_type final {
public:
    explicit impl_type(std::pmr::memory_resource* memory)
        : resource_(memory),
          decoder_({.resource_ = memory}) {}

    std::pmr::memory_resource* resource_;
    detail::hpack_decoder decoder_;
};

void hpack_decoder::impl_deleter_type::operator()(impl_type* value) const noexcept {
    if (value != nullptr) {
        detail::destroy_http_pmr_object(value, value->resource_);
    }
}

hpack_decoder::hpack_decoder(hpack_decoder_options options) {
    auto* resource = detail::http_pmr_resource_or_default(options.resource_);
    impl_.reset(detail::construct_http_pmr_object<impl_type>(resource, resource));
}
hpack_decoder::~hpack_decoder() = default;
hpack_decoder::hpack_decoder(hpack_decoder&&) noexcept = default;
hpack_decoder& hpack_decoder::operator=(hpack_decoder&&) noexcept = default;

void hpack_decoder::set_max_dynamic_table_size(std::size_t bytes_value) {
    impl_->decoder_.set_max_dynamic_table_size(bytes_value);
}

hpack_decode_result hpack_decoder::decode_with_callback(
    std::string_view block, void* target, header_callback_type callback_value) {
    return impl_->decoder_.decode(block, target, callback_value);
}

void hpack_encoder::encode_indexed(std::pmr::string& output, std::uint32_t index) {
    detail::hpack_encoder::encode_indexed(output, index);
}
void hpack_encoder::encode_dynamic_table_size_update(std::pmr::string& output, std::uint32_t maximum) {
    detail::hpack_encoder::encode_dynamic_table_size_update(output, maximum);
}
void hpack_encoder::encode_header(
    std::pmr::string& output, std::string_view name, std::string_view value) {
    detail::hpack_encoder::encode_header(output, name, value);
}
void hpack_encoder::encode_header_with_name_index(std::pmr::string& output, std::uint32_t name_index,
    std::string_view value, hpack_header_with_name_index_options options) {
    detail::hpack_encoder::encode_header_with_name_index(output, name_index, value, options.never_indexed_);
}
void hpack_encoder::encode_status(std::pmr::string& output, http_status_code status) {
    detail::hpack_encoder::encode_status(output, status);
}

}  // namespace ruvia
