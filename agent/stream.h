#ifndef DEBUGMGR_STREAM_H
#define DEBUGMGR_STREAM_H

#include <windows.h>

#include "server.h"

namespace debugmgr {

enum class stream_result { quit, lost };

stream_result serve_stream(HANDLE stream, device &target, uint32_t windows_ce_version, reporter notify, void *context);

}

#endif
