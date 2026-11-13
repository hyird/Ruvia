#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "ruvia/web/tls_config.h"

#include "tls/tls_credential.h"

namespace ruvia::detail {

class tls_certificate_snapshot final {
public:
    struct entry_type final {
        tls_certificate_id id_{};
        std::shared_ptr<const tls_certificate> certificate_;
    };

    [[nodiscard]] const tls_certificate* find(tls_certificate_id id) const noexcept {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &entry_type::id_);
        return (it != entries_.end() && it->id_ == id) ? it->certificate_.get() : nullptr;
    }

    [[nodiscard]] bool contains(tls_certificate_id id) const noexcept {
        return find(id) != nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    void insert_or_assign(tls_certificate_id id, std::shared_ptr<const tls_certificate> certificate) {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &entry_type::id_);
        if (it != entries_.end() && it->id_ == id) {
            it->certificate_ = std::move(certificate);
        } else {
            entries_.insert(it, entry_type{id, std::move(certificate)});
        }
    }

    void erase(tls_certificate_id id) noexcept {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &entry_type::id_);
        if (it != entries_.end() && it->id_ == id) {
            entries_.erase(it);
        }
    }

private:
    std::vector<entry_type> entries_;
};

struct tls_certificate_store_access final {
    [[nodiscard]] static std::shared_ptr<const tls_certificate_snapshot> snapshot(
        const tls_certificate_store& store) noexcept;
};

// Same validated loader used by fixed listener identities and incremental IDs.
[[nodiscard]] tls_certificate load_server_tls_certificate(const tls_identity_config& identity);

}  // namespace ruvia::detail
