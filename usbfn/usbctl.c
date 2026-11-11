#include <windows.h>

#include "activate.h"

#define DRIVER_PATH L"\\Storage Card\\brainshell\\usb\\usbserial.dll"
#define USBSER_SETTLE_MS 1500
#define LOG_PATH L"\\Storage Card\\brainshell\\usb\\usbctl.log"

static void append_log(const WCHAR *text)
{
    HANDLE file = CreateFileW(LOG_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    char line[512];
    DWORD written;
    int length;

    if (file == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(file, 0, NULL, FILE_END);
    for (length = 0; text[length] && length < (int)sizeof(line) - 2; length++)
        line[length] = (char)text[length];
    line[length++] = '\r';
    line[length++] = '\n';
    WriteFile(file, line, length, &written, NULL);
    CloseHandle(file);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show)
{
    WCHAR message[512];
    WCHAR current[128];
    BOOL ok = TRUE;
    LONG registered = 0;

    current[0] = 0;
    if (command_line && !lstrcmpiW(command_line, L"cdc")) {
        registered = usb_register_serial_client(DRIVER_PATH);
        ok = registered == ERROR_SUCCESS && usb_select_client(USB_SERIAL_CLIENT);
        wsprintfW(message, L"usbctl cdc: register=%d select=%d error=%u", (int)registered, ok ? 1 : 0, ok ? 0 : (unsigned)GetLastError());
    } else if (command_line && !lstrcmpiW(command_line, L"usbser")) {
        WCHAR device[16];

        ok = usb_select_client(USB_SHARP_SERIAL_CLIENT);
        Sleep(USBSER_SETTLE_MS);
        if (!usb_find_active_device(USB_SHARP_SERIAL_CLIENT, device, 16))
            lstrcpyW(device, L"(none)");
        wsprintfW(message, L"usbctl usbser: select=%d error=%u device=%s", ok ? 1 : 0, ok ? 0 : (unsigned)GetLastError(), device);
    } else if (command_line && !lstrcmpiW(command_line, L"msc")) {
        ok = usb_select_client(USB_MASS_STORAGE_CLIENT);
        wsprintfW(message, L"usbctl msc: select=%d error=%u", ok ? 1 : 0, ok ? 0 : (unsigned)GetLastError());
    } else {
        ok = usb_current_client(current, 128);
        wsprintfW(message, L"usbctl status: query=%d client=\"%s\" error=%u", ok ? 1 : 0, current, ok ? 0 : (unsigned)GetLastError());
    }
    append_log(message);
    if (!usb_current_client(current, 128))
        lstrcpyW(current, L"(unknown)");
    wsprintfW(message + lstrlenW(message), L"\r\ncurrent client: %s", current);
    append_log(message);
    MessageBoxW(NULL, message, L"usbctl", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
    return ok ? 0 : 1;
}
