#pragma once

#include <exception>

namespace ruvia::test {

template <typename fn_type>
[[nodiscard]] inline bool throws_on(fn_type&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

}  // namespace ruvia::test
