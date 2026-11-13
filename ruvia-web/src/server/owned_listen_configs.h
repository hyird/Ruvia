#pragma once

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ruvia/web/server_config.h"

#include "server/http_server_listener.h"

namespace ruvia::detail {

// 在构造冷任务前接管整批 socket，并拥有全部借用地址及配置副本。
// load() 将全部 socket 一次性交给唯一 loader；任何失败只关闭每个句柄一次。
class owned_listen_configs final {
public:
    explicit owned_listen_configs(listen_config config);
    explicit owned_listen_configs(std::span<const listen_config> configs);
    owned_listen_configs(owned_listen_configs&& other) noexcept
        : configs_(std::move(other.configs_)),
          addresses_(std::move(other.addresses_)),
          inherited_owned_(std::exchange(other.inherited_owned_, false)) {}
    owned_listen_configs(const owned_listen_configs&) = delete;
    owned_listen_configs& operator=(const owned_listen_configs&) = delete;
    owned_listen_configs& operator=(owned_listen_configs&&) = delete;
    ~owned_listen_configs();

    [[nodiscard]] std::vector<http_server_listener_definition> load();
    [[nodiscard]] bool needs_tls_preparation() const noexcept;
    std::vector<listen_config> configs_{};

private:
    void own_addresses();
    std::vector<std::string> addresses_{};
    bool inherited_owned_{true};
};

}  // namespace ruvia::detail
