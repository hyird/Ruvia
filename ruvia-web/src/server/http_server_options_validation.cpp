#include "server/http_server_options_validation.h"

#include <bit>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/core/memory/pmr_resource.h"

#include "server/http_server_options.h"
#include "util/config_validation.h"

namespace ruvia::detail {

void http_server_options::set_server(const server_config& config) {
    validate_server_limits(config);
    server_ = config;
}

void http_server_options::set_compression(compression_config config) {
    ensure_positive_size(
        config.min_bytes_, "compression minimum size must be greater than zero");
    if (config.sync_bytes_ < config.min_bytes_) {
        throw std::invalid_argument(
            "compression synchronous size must not be smaller than the minimum size");
    }
    if (config.max_bytes_ < config.sync_bytes_) {
        throw std::invalid_argument(
            "compression maximum size must not be smaller than the synchronous size");
    }
    compression_ = std::move(config);
}

void http_server_options::set_cors(const cors_config& config) {
    cors_ = make_cors_options(config, process_resource());
}

void http_server_options::set_rate_limit(const rate_limit_config& config) {
    validate_rate_limit_rule(config.rule_);
    if (!std::has_single_bit(config.capacity_per_worker_)) {
        throw std::invalid_argument(
            "rate-limit capacity per worker must be a power of two");
    }
    default_rate_limit_per_worker_ = config.rule_;
    rate_limit_capacity_per_worker_ = config.capacity_per_worker_;
}

void http_server_options::set_trusted_proxies(const trusted_proxy_config& config) {
    trusted_proxy_set parsed(process_resource());
    parsed.trust_x_forwarded_proto(config.trust_x_forwarded_proto_);
    for (const auto& cidr : config.cidrs_) {
        const auto block = parse_trusted_proxy_block(cidr);
        if (block.index() != 0) {
            // A typo here would silently trust nothing and leave every
            // client identified as the proxy, so it fails startup instead.
            throw std::invalid_argument(
                "trusted proxy must be an IP address or CIDR block");
        }
        parsed.add(std::get<0>(block));
    }
    trusted_proxies_ = std::move(parsed);
}

}  // namespace ruvia::detail
