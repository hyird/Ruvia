#pragma once

#include "ruvia/web/detail/server/HttpServerAutoHttps.h"
#include "ruvia/web/detail/server/HttpServerAlpn.h"
#include "ruvia/web/detail/server/HttpServerCleartextHttp2.h"
#include "ruvia/web/detail/server/HttpServerConnectionGuards.h"
#include "ruvia/web/detail/server/HttpServerHttp2UpgradeRoute.h"
#include "ruvia/web/detail/server/HttpServerIdleWorkSet.h"
#include "ruvia/web/detail/server/HttpServerRequestState.h"
#include "ruvia/web/detail/server/HttpServerResponseStreamRoute.h"
#include "ruvia/web/detail/server/HttpServerResponseState.h"
#include "ruvia/web/detail/server/HttpServerWebSocketRoute.h"
#include "ruvia/web/detail/server/HttpServerStreamBodyRoute.h"
#include "ruvia/core/detail/SocketUtils.h"
