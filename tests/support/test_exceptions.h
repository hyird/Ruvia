#pragma once

#include <exception>

namespace ruvia::testing {

template <typename fn_type>
[[nodiscard]] bool throws_on(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace ruvia::testing
