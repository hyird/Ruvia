#pragma once

#include "ruvia/core/tcp_socket_options.h"

namespace ruvia {

// Socket policies applied to every outbound TCP connection of one client.
struct client_tcp_config final {
    tcp_no_delay_policy no_delay_{tcp_no_delay_policy::enable};
    tcp_keep_alive_policy keep_alive_{tcp_keep_alive_policy::enable};
};

}  // namespace ruvia
