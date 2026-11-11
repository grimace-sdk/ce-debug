#include <windows.h>
#include <winsock.h>

#include "device_ce.h"
#include "mailbox.h"
#include "server.h"
#include "status_window.h"

namespace {

constexpr const wchar_t *settings_key = L"Software\\debugmgr";
constexpr const wchar_t *transport_value = L"Transport";
constexpr const wchar_t *mailbox_transport = L"mailbox";
constexpr const wchar_t *instance_mutex_name = L"debugmgr";
constexpr DWORD handover_wait_milliseconds = 10000;

bool registry_selects_mailbox() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, settings_key, 0, 0, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[16] = {};
    DWORD type = 0;
    DWORD size = sizeof(value) - sizeof(wchar_t);
    bool selected = RegQueryValueExW(key, transport_value, nullptr, &type, reinterpret_cast<BYTE *>(value), &size) == ERROR_SUCCESS &&
                    type == REG_SZ && _wcsicmp(value, mailbox_transport) == 0;
    RegCloseKey(key);
    return selected;
}

bool command_line_selects_mailbox(const wchar_t *command_line) {
    return wcsstr(command_line, mailbox_transport) != nullptr;
}

bool become_only_instance(HANDLE instance_lock) {
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND running = FindWindowW(debugmgr::status_window::class_name, nullptr);
        if (running) {
            PostMessageW(running, RegisterWindowMessageW(debugmgr::show_message_name), 0, 0);
        }
    }
    return WaitForSingleObject(instance_lock, handover_wait_milliseconds) != WAIT_TIMEOUT;
}

struct server_context {
    debugmgr::shared_status *status;
    HWND window;
    uint16_t port;
    bool mailbox;
};

void record(void *context, const debugmgr::report &happened) {
    server_context &server = *static_cast<server_context *>(context);
    debugmgr::status_snapshot &state = server.status->state;
    EnterCriticalSection(&server.status->lock);
    switch (happened.kind) {
    case debugmgr::event::listening:
        state.listening = true;
        state.failed = false;
        break;
    case debugmgr::event::listen_failed:
        state.failed = true;
        break;
    case debugmgr::event::connected:
    case debugmgr::event::replaced:
        state.connected = true;
        state.client = happened.address;
        state.connected_tick = GetTickCount();
        break;
    case debugmgr::event::request:
        state.requests++;
        if (happened.status != debugmgr::status_ok) {
            state.failures++;
        }
        state.last_command = happened.command;
        state.last_status = happened.status;
        state.last_request_tick = GetTickCount();
        state.bytes_in += happened.bytes_in;
        state.bytes_out += happened.bytes_out;
        break;
    case debugmgr::event::idle_closed:
    case debugmgr::event::disconnected:
        state.connected = false;
        break;
    }
    LeaveCriticalSection(&server.status->lock);
    PostMessageW(server.window, debugmgr::status_changed, 0, 0);
}

DWORD WINAPI run_server(void *parameter) {
    server_context &server = *static_cast<server_context *>(parameter);
    debugmgr::ce_device device;
    bool finished = server.mailbox ? debugmgr::serve_mailbox(device, _WIN32_WCE, record, &server)
                                   : debugmgr::serve(server.port, device, _WIN32_WCE, record, &server);
    if (finished) {
        PostMessageW(server.window, debugmgr::stop_requested, 0, 0);
    }
    return 0;
}

}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show) {
    bool mailbox = command_line_selects_mailbox(command_line) || registry_selects_mailbox();
    long requested_port = _wtol(command_line);
    uint16_t port = requested_port > 0 && requested_port < 65536 ? static_cast<uint16_t>(requested_port) : debugmgr::default_port;

    HANDLE instance_lock = CreateMutexW(nullptr, FALSE, instance_mutex_name);
    if (!instance_lock || !become_only_instance(instance_lock)) {
        return 0;
    }

    WSADATA winsock;
    if (WSAStartup(MAKEWORD(1, 1), &winsock) != 0) {
        MessageBoxW(nullptr, L"Winsock isn't available.", L"debugmgr", MB_OK | MB_ICONERROR);
        return 1;
    }

    debugmgr::shared_status status;
    InitializeCriticalSection(&status.lock);
    debugmgr::status_window window(status, port, mailbox);
    if (!window.open(instance)) {
        return 1;
    }

    server_context server{&status, window.handle(), port, mailbox};
    HANDLE thread = CreateThread(nullptr, 0, run_server, &server, 0, nullptr);
    if (!thread) {
        return 1;
    }
    CloseHandle(thread);
    int result = grimace::run_message_loop();
    WSACleanup();
    return result;
}
