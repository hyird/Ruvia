#include "ruvia/http/hpack.h"

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"

#include "http2/http2_hpack.h"

namespace ruvia {

void hpack_decoder::impl_deleter_type::operator()(detail::hpack_decoder* value) const noexcept {
    if (value != nullptr) {
        detail::destroy_http_pmr_object(detail::http_resolved_pmr_resource_tag{}, value, value->resource());
    }
}

hpack_decoder::hpack_decoder(hpack_decoder_options options) {
    auto* resource = detail::http_pmr_resource_or_default(options.resource_);
    impl_.reset(detail::construct_http_pmr_object<detail::hpack_decoder>(
        detail::http_resolved_pmr_resource_tag{}, resource, resource));
}
hpack_decoder::~hpack_decoder() = default;
hpack_decoder::hpack_decoder(hpack_decoder&&) noexcept = default;
hpack_decoder& hpack_decoder::operator=(hpack_decoder&&) noexcept = default;

void hpack_decoder::set_max_dynamic_table_size(std::size_t bytes_value) {
    impl_->set_max_dynamic_table_size(bytes_value);
}

std::optional<hpack_decode_error> hpack_decoder::decode_with_callback(
    std::string_view block, void* target, header_callback_type callback_value) {
    return impl_->decode(block, target, callback_value);
}

}  // namespace ruvia
