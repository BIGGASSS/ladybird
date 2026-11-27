/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::Extensions {

// What the network rules of the installed extensions make of a request.
enum class NetworkRequestOutcome : u8 {
    Allow,
    Block,
    Redirect,
    UpgradeScheme,
};

}
