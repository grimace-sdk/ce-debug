#ifndef DEBUGMGR_USB_FUNCTION_H
#define DEBUGMGR_USB_FUNCTION_H

#include <windows.h>

#include "server.h"
#include "status_window.h"

namespace debugmgr {

enum class usb_mode { none, sharp_serial, cdc_serial, rndis, automatic };

usb_mode usb_mode_named(const wchar_t *name);

struct usb_session {
    usb_mode mode;
    shared_status *status;
    HWND window;
    reporter notify;
    void *context;
};

bool serve_usb(usb_session &session, device &target, uint32_t windows_ce_version);
void usb_stopped(const status_snapshot &final_state);

}

#endif
