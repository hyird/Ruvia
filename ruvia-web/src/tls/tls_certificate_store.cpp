#include "tls/tls_certificate_store.h"

#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

#include <openssl/crypto.h>

#include "ruvia/core/blocking_pool.h"

#include "integration/runtime_control_gate.h"

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

// Never move a secret-bearing config between coroutine/lambda frames: an SSO
// move may leave bytes in its source. One stable, process-owned payload crosses
// the pool boundary, and every incoming/partially populated copy is cleansed.
struct owned_identity final {
    tls_identity_config value_;
    ~owned_identity() {
        const cleanse_identity cleanse{value_};
    }
};

using owned_identity_pointer = std::unique_ptr<owned_identity>;

void require_accepting_caller(const worker_ref& caller) {
    if (!caller.accepting()) {
        throw std::runtime_error("TLS certificate mutation caller is stopping");
    }
}

}  // namespace

struct tls_certificate_store::state final {
    detail::runtime_control_gate writer_;
    std::atomic<std::shared_ptr<const detail::tls_certificate_snapshot>> snapshot_{
        std::make_shared<const detail::tls_certificate_snapshot>()};

    [[nodiscard]] static task<void> insert_or_assign(
        std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id,
        owned_identity_pointer identity) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto prepared = co_await run_blocking(caller,
            [current = owner->snapshot_.load(std::memory_order_acquire), id,
                identity = std::move(identity)]() mutable {
                auto certificate = std::make_shared<const detail::tls_certificate>(
                    detail::load_server_tls_certificate(identity->value_));
                identity.reset();
                auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
                next->insert_or_assign(id, std::move(certificate));
                return next;
            });
        auto next = std::move(prepared).take_value();
        require_accepting_caller(caller);
        owner->snapshot_.store(std::move(next), std::memory_order_release);
    }

    [[nodiscard]] static task<bool> erase(
        std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto current = owner->snapshot_.load(std::memory_order_acquire);
        if (!current->contains(id)) {
            co_return false;
        }
        // Copying an arbitrarily large immutable map is real preparation, not
        // a synchronous erase disguised by posting a closure to the caller.
        auto prepared = co_await run_blocking(caller,
            [current = std::move(current), id] {
                auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
                next->erase(id);
                return next;
            });
        auto next = std::move(prepared).take_value();
        require_accepting_caller(caller);
        owner->snapshot_.store(std::move(next), std::memory_order_release);
        co_return true;
    }
};

tls_certificate_store::tls_certificate_store()
    : state_(std::make_shared<state>()) {}
tls_certificate_store::~tls_certificate_store() = default;

task<void> tls_certificate_store::insert_or_assign(
    worker_ref caller, tls_certificate_id id, tls_identity_config identity) {
    const cleanse_identity cleanse{identity};
    auto owner = state_;
    if (id == 0) {
        throw std::invalid_argument("TLS certificate ID zero is reserved");
    }
    auto owned = std::make_unique<owned_identity>();
    owned->value_.certificate_chain_file_ = std::move(identity.certificate_chain_file_);
    owned->value_.private_key_file_ = std::move(identity.private_key_file_);
    owned->value_.certificate_chain_pem_.assign(identity.certificate_chain_pem_);
    owned->value_.private_key_pem_.assign(identity.private_key_pem_);
    owned->value_.private_key_password_.assign(identity.private_key_password_);
    return state::insert_or_assign(std::move(owner), std::move(caller), id, std::move(owned));
}

task<bool> tls_certificate_store::erase(worker_ref caller, tls_certificate_id id) {
    return state::erase(state_, std::move(caller), id);
}

bool tls_certificate_store::contains(tls_certificate_id id) const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->contains(id);
}

std::size_t tls_certificate_store::size() const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->size();
}

bool tls_certificate_selection::offers_signature_algorithm(std::uint16_t algorithm) const noexcept {
    return std::ranges::find(signature_algorithms_, algorithm) != signature_algorithms_.end();
}

std::shared_ptr<const detail::tls_certificate_snapshot> detail::tls_certificate_store_access::snapshot(
    const tls_certificate_store& store) noexcept {
    return store.state_->snapshot_.load(std::memory_order_acquire);
}

}  // namespace ruvia
