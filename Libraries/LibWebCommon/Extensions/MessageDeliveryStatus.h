/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::Extensions {

// How the listeners for an extension's message in a process answered it.
enum class MessageDeliveryStatus : u8 {
    NoListener,
    NoResponse,
    WillRespond,
    Responded,
    // The response of a listener that said it would respond later.
    RespondedLater,
};

}
