#include <utility>

#include "ruvia/http/http_native_path.h"
#include "ruvia/web/server_config.h"

#include "app/app_config_mutation.h"
#include "http/static_root_options_validation.h"
#include "util/config_validation.h"

namespace ruvia {

namespace {

[[nodiscard]] detail::static_root_precompression_options make_static_root_precompression_options(
    const document_root_config& config) {
    detail::ensure_positive_size(config.precompress_min_bytes_,
        "document root precompression minimum size must be greater than zero");
    if (config.precompress_max_bytes_ < config.precompress_min_bytes_) {
        throw std::invalid_argument(
            "document root precompression maximum size must not be smaller than the minimum size");
    }
    return detail::static_root_precompression_options{
        .gzip_ = config.precompress_gzip_,
        .brotli_ = config.precompress_brotli_,
        .zstd_ = config.precompress_zstd_,
        .min_bytes_ = config.precompress_min_bytes_,
        .max_bytes_ = config.precompress_max_bytes_,
    };
}

}  // namespace

application& application::compression(compression_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change compression config while app is running",
        [config = std::move(config)](detail::app_state& state_value) mutable {
            state_value.options_.set_compression(std::move(config));
        });
}

application& application::compression(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change compression config while app is running",
        [](detail::app_state& state_value) { state_value.options_.compression_.reset(); });
}

application& application::cors(cors_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change CORS config while app is running", [&config](detail::app_state& state_value) {
            state_value.options_.set_cors(config);
        });
}
application& application::cors(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change CORS config while app is running",
        [](detail::app_state& state_value) { state_value.options_.cors_.reset(); });
}

application& application::document_root(document_root_config config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change document root while app is running", [&config](detail::app_state& state_value) {
            if (config.root_.empty()) {
                throw std::invalid_argument("document root must not be empty");
            }
            detail::ensure_positive_duration(config.refresh_interval_,
                "document root refresh interval must be greater than zero");
            detail::validate_static_root_options(config.static_options_);
            const auto precompression = make_static_root_precompression_options(config);

            detail::app_document_root_config replacement(
                process_resource(), detail::store_validated_static_root_config(
                                        config.static_options_, process_resource()));
            http_assign_native_path(replacement.root_, config.root_);
            replacement.refresh_interval_ = config.refresh_interval_;
            replacement.precompression_ = precompression;
            replacement.resident_ = detail::static_root_resident_options{
                .max_file_bytes_ = config.static_options_.resident_max_bytes_.value_or(std::size_t{64} * 1024),
                .budget_bytes_ = config.static_options_.resident_budget_bytes_.value_or(std::size_t{16} * 1024 * 1024),
            };

            state_value.document_root_config_ = std::move(replacement);
        });
}

application& application::document_root(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change document root while app is running",
        [](detail::app_state& state_value) { state_value.document_root_config_.reset(); });
}

application& application::use_middleware(detail::owned_middleware_descriptor descriptor) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot add app middleware while app is running",
        [&descriptor](detail::app_state& state_value) {
            state_value.configuration_.add_middleware(std::move(descriptor));
        });
}

application& application::blocking_pool(blocking_pool_options config) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the blocking pool while app is running",
        [&config](detail::app_state& state_value) { state_value.blocking_pool_ = config; });
}

application& application::blocking_pool(std::nullptr_t) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change the blocking pool while app is running",
        [](detail::app_state& state_value) { state_value.blocking_pool_.reset(); });
}

application& application::use_worker_state_definition(detail::worker_state_definition definition) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot register worker state while app is running",
        [&definition](detail::app_state& state_value) {
            state_value.configuration_.add_worker_state(std::move(definition));
        });
}

application& application::on_error(http_error_handler handler) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change error handler while app is running",
        [handler = std::move(handler)](
            detail::app_state& state_value) mutable { state_value.configuration_.on_error(std::move(handler)); });
}

application& application::on_not_found(http_not_found_handler handler) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change not found handler while app is running",
        [handler = std::move(handler)](
            detail::app_state& state_value) mutable { state_value.configuration_.on_not_found(std::move(handler)); });
}

application& application::on_error(scoped_error_handler_options options) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change error handler while app is running",
        [options = std::move(options)](detail::app_state& state_value) mutable {
            state_value.configuration_.on_error(std::move(options));
        });
}

application& application::on_not_found(scoped_not_found_handler_options options) {
    return detail::mutate_stopped_app(*this, *state_,
        "cannot change not found handler while app is running",
        [options = std::move(options)](detail::app_state& state_value) mutable {
            state_value.configuration_.on_not_found(std::move(options));
        });
}

}  // namespace ruvia
