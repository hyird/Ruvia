#pragma once

#include "dns_resolver.h"

namespace ruvia::detail {

inline constexpr auto client_connection_attempt_delay = ::ruvia::happy_eyeballs_attempt_delay;

template <typename results_type>
using client_address_sequence = ::ruvia::address_family_interleaver<results_type>;

using client_happy_eyeballs_result = ::ruvia::happy_eyeballs_result;
using client_happy_eyeballs = ::ruvia::happy_eyeballs_dialer;

}  // namespace ruvia::detail
