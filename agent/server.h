#ifndef DEBUGMGR_SERVER_H
#define DEBUGMGR_SERVER_H

#include <stdint.h>

#include "protocol.h"

namespace debugmgr {

enum class event { listening, listen_failed, connected, replaced, request, idle_closed, disconnected };

struct report {
    event kind;
    uint32_t address;
    uint16_t command;
    uint32_t status;
    uint32_t bytes_in;
    uint32_t bytes_out;
};

using reporter = void (*)(void *context, const report &happened);

constexpr uint32_t idle_timeout_milliseconds = 120000;

bool serve(uint16_t port, device &target, uint32_t windows_ce_version, reporter notify, void *context,
           uint32_t idle_timeout = idle_timeout_milliseconds);

}

#endif
