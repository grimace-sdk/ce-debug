#ifndef DEBUGMGR_SERVER_H
#define DEBUGMGR_SERVER_H

#include <stdint.h>

#include "protocol.h"

namespace debugmgr {

enum class event { listening, listen_failed, connected, request, disconnected };

struct report {
    event kind;
    uint32_t address;
    uint16_t command;
    uint32_t status;
};

using reporter = void (*)(void *context, const report &happened);

bool serve(uint16_t port, device &target, uint32_t windows_ce_version, reporter notify, void *context);

}

#endif
