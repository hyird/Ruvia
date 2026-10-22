#include "ruvia/http/hpack.h"

#include "ruvia/http/detail/util/http_pmr_object.h"
#include "ruvia/http/detail/util/pmr_resource.h"

#include "http2/http2_hpack.h"

namespace ruvia {

class hpack_decoder::impl_type final {
public:
    explicit impl_type(std::pmr::memory_resource* memory)
        : resource_(memory),
          decoder_(memory) {}

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

std::optional<hpack_decode_error> hpack_decoder::decode_with_callback(
    std::string_view block, void* target, header_callback_type callback_value) {
    return impl_->decoder_.decode(block, target, callback_value);
}

}  // namespace ruvia
