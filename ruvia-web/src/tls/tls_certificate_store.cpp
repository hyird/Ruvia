#include "tls/tls_certificate_store.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <openssl/crypto.h>

namespace ruvia {
namespace {

struct cleanse_identity final {
    tls_identity_config& identity_;
    ~cleanse_identity() {
        for (auto* secret : {&identity_.certificate_chain_pem_, &identity_.private_key_pem_,
                 &identity_.private_key_password_}) {
            if (!secret->empty()) {
                OPENSSL_cleanse(secret->data(), secret->size());
                secret->clear();
            }
        }
    }
};

}  // namespace

struct tls_certificate_store::state final {
    std::mutex writer_;
    std::atomic<std::shared_ptr<const detail::tls_certificate_snapshot>> snapshot_{
        std::make_shared<const detail::tls_certificate_snapshot>()};
};

tls_certificate_store::tls_certificate_store()
    : state_(std::make_unique<state>()) {}
tls_certificate_store::~tls_certificate_store() = default;

void tls_certificate_store::insert_or_assign(tls_certificate_id id, tls_identity_config identity) {
    const cleanse_identity cleanse{identity};
    if (id == 0) {
        throw std::invalid_argument("TLS certificate ID zero is reserved");
    }
    auto certificate = std::make_shared<const detail::tls_certificate>(detail::load_server_tls_certificate(identity));
    const std::lock_guard lock(state_->writer_);
    auto next = std::make_shared<detail::tls_certificate_snapshot>(*state_->snapshot_.load(std::memory_order_acquire));
    next->insert_or_assign(id, std::move(certificate));
    state_->snapshot_.store(std::move(next), std::memory_order_release);
}

bool tls_certificate_store::erase(tls_certificate_id id) {
    const std::lock_guard lock(state_->writer_);
    const auto current = state_->snapshot_.load(std::memory_order_acquire);
    if (!current->contains(id)) {
        return false;
    }
    auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
    next->erase(id);
    state_->snapshot_.store(std::move(next), std::memory_order_release);
    return true;
}

bool tls_certificate_store::contains(tls_certificate_id id) const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->contains(id);
}

std::size_t tls_certificate_store::size() const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->size();
}

bool tls_certificate_selection::offers_signature_algorithm(std::uint16_t algorithm) const noexcept {
    for (std::size_t i = 0; i + 1 < signature_algorithms_.size(); i += 2) {
        if ((std::to_integer<unsigned int>(signature_algorithms_[i]) << 8 |
                std::to_integer<unsigned int>(signature_algorithms_[i + 1])) == algorithm) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<const detail::tls_certificate_snapshot> detail::tls_certificate_store_access::snapshot(
    const tls_certificate_store& store) noexcept {
    return store.state_->snapshot_.load(std::memory_order_acquire);
}

}  // namespace ruvia
