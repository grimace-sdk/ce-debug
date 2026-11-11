#include <windows.h>
#include <winsock.h>

#include "device_ce.h"
#include "mailbox.h"
#include "server.h"
#include "status_window.h"
#include "stream.h"
#ifdef GRIMACE_PLATFORM_BRAIN
#include "brain_device.h"
#include "usb_function.h"
#endif

namespace {

constexpr const wchar_t *settings_key = L"Software\\debugmgr";
constexpr const wchar_t *transport_value = L"Transport";
constexpr const wchar_t *instance_mutex_name = L"debugmgr";
constexpr DWORD handover_wait_milliseconds = 10000;
constexpr DWORD stream_retry_milliseconds = 1000;

struct selection {
    debugmgr::transport_kind kind = debugmgr::transport_kind::network;
    uint16_t port = debugmgr::default_port;
    grimace::wstring<32> stream;
    bool chosen = false;
#ifdef GRIMACE_PLATFORM_BRAIN
    debugmgr::usb_mode usb = debugmgr::usb_mode::none;
#endif
};

void choose(selection &chosen, const wchar_t *word) {
    long number = _wtol(word);
    if (number > 0 && number < 65536) {
        chosen.kind = debugmgr::transport_kind::network;
        chosen.port = static_cast<uint16_t>(number);
    } else if (!_wcsicmp(word, L"network")) {
        chosen.kind = debugmgr::transport_kind::network;
    } else if (!_wcsicmp(word, L"mailbox")) {
        chosen.kind = debugmgr::transport_kind::mailbox;
    } else if (!_wcsnicmp(word, L"stream=", 7) && word[7]) {
        chosen.kind = debugmgr::transport_kind::stream;
        chosen.stream = word + 7;
    } else {
#ifdef GRIMACE_PLATFORM_BRAIN
        debugmgr::usb_mode usb = debugmgr::usb_mode_named(word);
        if (usb == debugmgr::usb_mode::none) {
            return;
        }
        chosen.usb = usb;
#else
        return;
#endif
    }
    chosen.chosen = true;
}

void choose_all(selection &chosen, const wchar_t *words) {
    wchar_t word[48];
    while (*words) {
        while (*words == L' ') {
            words++;
        }
        int length = 0;
        while (words[length] && words[length] != L' ' && length < 47) {
            word[length] = words[length];
            length++;
        }
        word[length] = 0;
        if (length) {
            choose(chosen, word);
        }
        words += length;
        while (*words && *words != L' ') {
            words++;
        }
    }
}

void choose_from_registry(selection &chosen) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, settings_key, 0, 0, &key) != ERROR_SUCCESS) {
        return;
    }
    wchar_t value[64] = {};
    DWORD type = 0;
    DWORD size = sizeof(value) - sizeof(wchar_t);
    if (RegQueryValueExW(key, transport_value, nullptr, &type, reinterpret_cast<BYTE *>(value), &size) == ERROR_SUCCESS && type == REG_SZ) {
        choose_all(chosen, value);
    }
    RegCloseKey(key);
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
    selection chosen;
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

bool serve_stream_device(server_context &server, debugmgr::device &device) {
    for (;;) {
        HANDLE stream = CreateFileW(server.chosen.stream.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (stream == INVALID_HANDLE_VALUE) {
            Sleep(stream_retry_milliseconds);
            continue;
        }
        debugmgr::stream_result result = debugmgr::serve_stream(stream, device, _WIN32_WCE, record, &server);
        CloseHandle(stream);
        if (result == debugmgr::stream_result::quit) {
            return true;
        }
        Sleep(stream_retry_milliseconds);
    }
}

DWORD WINAPI run_server(void *parameter) {
    server_context &server = *static_cast<server_context *>(parameter);
#ifdef GRIMACE_PLATFORM_BRAIN
    debugmgr::brain_device device;
    if (server.chosen.usb != debugmgr::usb_mode::none) {
        debugmgr::usb_session session{server.chosen.usb, server.status, server.window, record, &server};
        if (debugmgr::serve_usb(session, device, _WIN32_WCE)) {
            PostMessageW(server.window, debugmgr::stop_requested, 0, 0);
        }
        return 0;
    }
#else
    debugmgr::ce_device device;
#endif
    bool finished = false;
    switch (server.chosen.kind) {
    case debugmgr::transport_kind::network:
        finished = debugmgr::serve(server.chosen.port, device, _WIN32_WCE, record, &server);
        break;
    case debugmgr::transport_kind::mailbox:
        finished = debugmgr::serve_mailbox(device, _WIN32_WCE, record, &server);
        break;
    case debugmgr::transport_kind::stream:
        finished = serve_stream_device(server, device);
        break;
    }
    if (finished) {
        PostMessageW(server.window, debugmgr::stop_requested, 0, 0);
    }
    return 0;
}

}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show) {
    selection chosen;
    choose_all(chosen, command_line);
    if (!chosen.chosen) {
        choose_from_registry(chosen);
    }
#ifdef GRIMACE_PLATFORM_BRAIN
    if (!chosen.chosen) {
        chosen.usb = debugmgr::usb_mode::rndis;
    }
#endif

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
    status.state.transport = chosen.kind;
    status.state.port = chosen.port;
    status.state.stream = chosen.stream;
    debugmgr::status_window window(status);
    if (!window.open(instance)) {
        return 1;
    }

    server_context server{&status, window.handle(), chosen};
    HANDLE thread = CreateThread(nullptr, 0, run_server, &server, 0, nullptr);
    if (!thread) {
        return 1;
    }
    CloseHandle(thread);
    int result = grimace::run_message_loop();
#ifdef GRIMACE_PLATFORM_BRAIN
    if (chosen.usb != debugmgr::usb_mode::none) {
        EnterCriticalSection(&status.lock);
        debugmgr::status_snapshot final_state = status.state;
        LeaveCriticalSection(&status.lock);
        debugmgr::usb_stopped(final_state);
    }
#endif
    WSACleanup();
    return result;
}
