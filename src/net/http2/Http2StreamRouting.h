#pragma once

#include "../../router/RouteResolution.h"
#include "ruvia/http/HttpCommon.h"

namespace ruvia::detail {

class Http2StreamRouting final {
public:
    [[nodiscard]] RouteMatch& match() noexcept {
        return match_;
    }

    [[nodiscard]] const RouteResolution& resolution() const noexcept {
        return resolution_;
    }

    [[nodiscard]] RequestBodyMode bodyMode() const noexcept {
        return bodyMode_;
    }

    [[nodiscard]] bool usesStreamRequestBody() const noexcept {
        return bodyMode_ == RequestBodyMode::kStream;
    }

    void resetToBuffered() noexcept {
        match_.clear();
        resolution_ = {};
        bodyMode_ = RequestBodyMode::kBuffered;
    }

    void setResolution(RouteResolution resolution) noexcept {
        resolution_ = resolution;
    }

    void setBodyMode(RequestBodyMode bodyMode) noexcept {
        bodyMode_ = bodyMode;
    }

private:
    RouteMatch match_;
    RouteResolution resolution_;
    RequestBodyMode bodyMode_{RequestBodyMode::kBuffered};
};

}  // namespace ruvia::detail
