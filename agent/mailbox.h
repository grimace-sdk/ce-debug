#ifndef DEBUGMGR_MAILBOX_H
#define DEBUGMGR_MAILBOX_H

#include "server.h"

namespace debugmgr {

bool serve_mailbox(device &target, uint32_t windows_ce_version, reporter notify, void *context);

}

#endif
