#ifndef BRAIN_USB_ACTIVATE_H
#define BRAIN_USB_ACTIVATE_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_SERIAL_CLIENT L"Brain_CDC_Class"
#define USB_MASS_STORAGE_CLIENT L"Mass_Storage_Class"
#define USB_SERIAL_DEVICE L"USC1:"
#define USB_SHARP_SERIAL_CLIENT L"USBSER_Class"
#define USB_RNDIS_CLIENT L"RNDIS"

LONG usb_register_serial_client(const WCHAR *driver_path);
LONG usb_register_rndis_client(void);
BOOL usb_current_client(WCHAR *name, DWORD name_characters);
BOOL usb_select_client(const WCHAR *name);
BOOL usb_find_active_device(const WCHAR *client, WCHAR *name, DWORD name_characters);

#ifdef __cplusplus
}
#endif

#endif
