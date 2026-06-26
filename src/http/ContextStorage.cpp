#include "ruvia/http/Context.h"

namespace ruvia {

std::pmr::string& Context::decodedBody() const {
    if (decodedBody_ == nullptr) {
        decodedBody_ = &memory_.emplace<std::pmr::string>(resource());
    }
    return *decodedBody_;
}

std::pmr::string& Context::sessionIdStorage() {
    if (sessionId_ == nullptr) {
        sessionId_ = &memory_.emplace<std::pmr::string>(resource());
    }
    return *sessionId_;
}

std::pmr::string& Context::sessionDataStorage() {
    if (sessionData_ == nullptr) {
        sessionData_ = &memory_.emplace<std::pmr::string>(resource());
    }
    return *sessionData_;
}

}  // namespace ruvia
