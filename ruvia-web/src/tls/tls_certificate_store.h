#pragma once

#include <map>
#include <memory>

#include "ruvia/web/tls_config.h"

#include "tls/tls_credential.h"

namespace ruvia::detail {

using tls_certificate_snapshot = std::map<tls_certificate_id, std::shared_ptr<const tls_certificate>>;

struct tls_certificate_store_access final {
    [[nodiscard]] static std::shared_ptr<const tls_certificate_snapshot> snapshot(
        const tls_certificate_store& store) noexcept;
};

// Same validated loader used by fixed listener identities and incremental IDs.
[[nodiscard]] tls_certificate load_server_tls_certificate(const tls_identity_config& identity);

}  // namespace ruvia::detail
