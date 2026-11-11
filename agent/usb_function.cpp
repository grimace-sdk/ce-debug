#include "usb_function.h"

#include <msgqueue.h>
#include <pm.h>
#include <winsock.h>

#include <grimace/cxx/format.h>

#include "../usbfn/activate.h"
#include "../usbfn/events.h"
#include "stream.h"

namespace debugmgr {

namespace {

constexpr DWORD open_attempts = 30;
constexpr DWORD write_timeout_milliseconds = 3000;
constexpr DWORD network_fallback_milliseconds = 120000;
constexpr DWORD event_poll_milliseconds = 2000;
constexpr DWORD power_queue_messages = 4;
constexpr const wchar_t *serial_mode = L"USB serial";
constexpr const wchar_t *network_mode = L"Network over USB (Brainwave or brain-net)";

volatile LONG network_connections = 0;
volatile HANDLE watched_port = INVALID_HANDLE_VALUE;
volatile bool owns_rndis = false;
bool rndis_active = false;
usb_session *current_session = nullptr;
device *current_device = nullptr;
uint32_t current_version = 0;

grimace::wstring<MAX_PATH> beside_program(const wchar_t *name) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) {
        slash[1] = 0;
    }
    grimace::wstring<MAX_PATH> result(path);
    result.append(name);
    return result;
}

void append_log(const wchar_t *text) {
    grimace::wstring<MAX_PATH> path = beside_program(L"debugmgr.log");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    char line[256];
    int length = 0;
    while (text[length] && length < static_cast<int>(sizeof(line)) - 2) {
        line[length] = static_cast<char>(text[length]);
        length++;
    }
    line[length++] = '\r';
    line[length++] = '\n';
    DWORD written = 0;
    SetFilePointer(file, 0, nullptr, FILE_END);
    WriteFile(file, line, length, &written, nullptr);
    CloseHandle(file);
}

void describe(usb_session &session, transport_kind kind, const wchar_t *stream, const wchar_t *mode, const wchar_t *note, bool restores) {
    EnterCriticalSection(&session.status->lock);
    status_snapshot &state = session.status->state;
    state.transport = kind;
    state.port = default_port;
    state.stream = stream ? stream : L"";
    state.mode = mode;
    state.note = note ? note : L"";
    state.stop_restores_storage = restores;
    LeaveCriticalSection(&session.status->lock);
    PostMessageW(session.window, status_changed, 0, 0);
}

void set_note(usb_session &session, const wchar_t *note) {
    EnterCriticalSection(&session.status->lock);
    session.status->state.note = note ? note : L"";
    LeaveCriticalSection(&session.status->lock);
    PostMessageW(session.window, status_changed, 0, 0);
}

bool current_client_is(const wchar_t *client) {
    wchar_t current[64] = L"";
    return usb_current_client(current, 64) && !lstrcmpiW(current, client);
}

HANDLE open_serial(usb_mode mode, wchar_t *name, DWORD name_characters) {
    if (mode == usb_mode::cdc_serial) {
        lstrcpynW(name, USB_SERIAL_DEVICE, name_characters);
        return CreateFileW(USB_SERIAL_DEVICE, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (!usb_find_active_device(USB_SHARP_SERIAL_CLIENT, name, name_characters)) {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE port = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (port == INVALID_HANDLE_VALUE) {
        return port;
    }
    DCB state{};
    state.DCBlength = sizeof(state);
    if (GetCommState(port, &state)) {
        state.fOutxCtsFlow = FALSE;
        state.fOutxDsrFlow = FALSE;
        state.fDsrSensitivity = FALSE;
        state.fOutX = FALSE;
        state.fInX = FALSE;
        state.fDtrControl = DTR_CONTROL_ENABLE;
        state.fRtsControl = RTS_CONTROL_ENABLE;
        SetCommState(port, &state);
    }
    COMMTIMEOUTS timeouts{};
    timeouts.WriteTotalTimeoutConstant = write_timeout_milliseconds;
    SetCommTimeouts(port, &timeouts);
    return port;
}

HANDLE activate_and_open(usb_session &session, usb_mode mode, wchar_t *name, DWORD name_characters) {
    HANDLE port = open_serial(mode, name, name_characters);
    if (port != INVALID_HANDLE_VALUE) {
        return port;
    }
    set_note(session, L"Switching USB to serial...");
    bool cdc = mode == usb_mode::cdc_serial;
    LONG registered = cdc ? usb_register_serial_client(beside_program(L"usbserial.dll").c_str()) : ERROR_SUCCESS;
    const wchar_t *wanted = cdc ? USB_SERIAL_CLIENT : USB_SHARP_SERIAL_CLIENT;
    BOOL selected = registered == ERROR_SUCCESS && (current_client_is(wanted) || usb_select_client(wanted));
    append_log(grimace::format<160>(L"activate: register=%d select=%d error=%u", static_cast<int>(registered), selected ? 1 : 0,
                                    selected ? 0u : static_cast<unsigned>(GetLastError()))
                   .c_str());
    set_note(session, selected ? L"Plug in the USB cable now." : L"Could not switch USB to serial.");
    for (DWORD attempt = 0; attempt < open_attempts; attempt++) {
        port = open_serial(mode, name, name_characters);
        if (port != INVALID_HANDLE_VALUE) {
            return port;
        }
        Sleep(1000);
    }
    return INVALID_HANDLE_VALUE;
}

void counting_reporter(void *context, const report &happened) {
    if (happened.kind == event::connected || happened.kind == event::replaced) {
        InterlockedIncrement(&network_connections);
    }
    usb_session &session = *static_cast<usb_session *>(context);
    session.notify(session.context, happened);
}

DWORD WINAPI network_thread(LPVOID) {
    if (serve(default_port, *current_device, current_version, counting_reporter, current_session)) {
        append_log(L"quit requested");
        PostMessageW(current_session->window, stop_requested, 0, 0);
    }
    return 0;
}

bool start_network(usb_session &session) {
    bool already = current_client_is(USB_RNDIS_CLIENT);
    LONG registered = usb_register_rndis_client();
    BOOL selected = registered == ERROR_SUCCESS && (already || usb_select_client(USB_RNDIS_CLIENT));
    append_log(grimace::format<160>(L"activate rndis: register=%d select=%d error=%u", static_cast<int>(registered), selected ? 1 : 0,
                                    selected ? 0u : static_cast<unsigned>(GetLastError()))
                   .c_str());
    if (!selected) {
        return false;
    }
    owns_rndis = !already;
    if (!rndis_active) {
        HANDLE thread = CreateThread(nullptr, 0, network_thread, nullptr, 0, nullptr);
        if (!thread) {
            return false;
        }
        CloseHandle(thread);
        rndis_active = true;
    }
    describe(session, transport_kind::network, nullptr, network_mode, already ? nullptr : L"Unplug and replug USB, then connect.", !already);
    if (already) {
        append_log(L"network: USB networking was already on; leaving it to its owner");
        return true;
    }
    DWORD started = GetTickCount();
    while (!network_connections && GetTickCount() - started < network_fallback_milliseconds) {
        Sleep(1000);
    }
    if (network_connections) {
        set_note(session, nullptr);
        return true;
    }
    append_log(L"network: no connection, falling back to serial");
    return false;
}

void append_events(const usb_serial_event_log &log, DWORD &logged) {
    grimace::wstring<MAX_PATH> path = beside_program(L"usb-events.log");
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    SetFilePointer(file, 0, nullptr, FILE_END);
    DWORD first = log.next_sequence > USB_SERIAL_EVENT_LIMIT ? log.next_sequence - USB_SERIAL_EVENT_LIMIT : 1;
    if (logged + 1 > first) {
        first = logged + 1;
    }
    for (DWORD sequence = first; sequence < log.next_sequence; sequence++) {
        const usb_serial_event &entry = log.events[(sequence - 1) % USB_SERIAL_EVENT_LIMIT];
        if (entry.sequence != sequence) {
            continue;
        }
        grimace::wstring<96> wide = grimace::format<96>(L"%06u #%u code=%u %08X %08X\r\n", static_cast<unsigned>(entry.tick),
                                                       static_cast<unsigned>(sequence), static_cast<unsigned>(entry.code),
                                                       static_cast<unsigned>(entry.first), static_cast<unsigned>(entry.second));
        char line[96];
        int length = 0;
        for (wchar_t character : wide.view()) {
            line[length++] = static_cast<char>(character);
        }
        DWORD written = 0;
        WriteFile(file, line, length, &written, nullptr);
        logged = sequence;
    }
    CloseHandle(file);
}

DWORD WINAPI event_thread(LPVOID) {
    DWORD logged = 0;
    usb_serial_event_log log;
    for (;;) {
        HANDLE port = watched_port;
        DWORD returned = 0;
        if (port != INVALID_HANDLE_VALUE &&
            DeviceIoControl(port, IOCTL_USBSERIAL_EVENTS, nullptr, 0, &log, sizeof(log), &returned, nullptr) && returned == sizeof(log)) {
            append_events(log, logged);
        }
        Sleep(event_poll_milliseconds);
    }
}

DWORD WINAPI power_thread(LPVOID) {
    MSGQUEUEOPTIONS options{};
    options.dwSize = sizeof(options);
    options.dwMaxMessages = power_queue_messages;
    options.cbMaxMessage = sizeof(POWER_BROADCAST) + MAX_PATH * sizeof(WCHAR);
    options.bReadAccess = TRUE;
    HANDLE queue = CreateMsgQueue(nullptr, &options);
    if (!queue || !RequestPowerNotifications(queue, PBT_TRANSITION)) {
        append_log(L"power notifications unavailable");
        return 0;
    }
    bool suspended = false;
    for (;;) {
        BYTE buffer[sizeof(POWER_BROADCAST) + MAX_PATH * sizeof(WCHAR)];
        DWORD read = 0;
        DWORD flags = 0;
        if (!ReadMsgQueue(queue, buffer, sizeof(buffer), &read, INFINITE, &flags)) {
            continue;
        }
        const POWER_BROADCAST *broadcast = reinterpret_cast<const POWER_BROADCAST *>(buffer);
        if (broadcast->Message != PBT_TRANSITION) {
            continue;
        }
        if (broadcast->Flags & POWER_STATE_SUSPEND) {
            suspended = true;
            append_log(L"suspending");
        } else if ((broadcast->Flags & POWER_STATE_ON) && suspended) {
            suspended = false;
            if (rndis_active && owns_rndis) {
                BOOL selected = usb_select_client(USB_RNDIS_CLIENT);
                append_log(grimace::format<64>(L"resumed: reselect rndis=%d error=%u", selected ? 1 : 0,
                                               selected ? 0u : static_cast<unsigned>(GetLastError()))
                               .c_str());
            } else {
                append_log(L"resumed");
            }
        }
    }
}

void start_thread(LPTHREAD_START_ROUTINE routine) {
    HANDLE thread = CreateThread(nullptr, 0, routine, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
}

}

usb_mode usb_mode_named(const wchar_t *name) {
    if (!_wcsicmp(name, L"usb")) {
        return usb_mode::sharp_serial;
    }
    if (!_wcsicmp(name, L"cdc")) {
        return usb_mode::cdc_serial;
    }
    if (!_wcsicmp(name, L"rndis")) {
        return usb_mode::rndis;
    }
    if (!_wcsicmp(name, L"auto")) {
        return usb_mode::automatic;
    }
    return usb_mode::none;
}

bool serve_usb(usb_session &session, device &target, uint32_t windows_ce_version) {
    current_session = &session;
    current_device = &target;
    current_version = windows_ce_version;
    usb_mode mode = session.mode;
    if (mode == usb_mode::automatic) {
        mode = current_client_is(USB_RNDIS_CLIENT) ? usb_mode::rndis : usb_mode::sharp_serial;
    }
    append_log(L"debugmgr starting");
    start_thread(power_thread);
    if (mode == usb_mode::cdc_serial) {
        start_thread(event_thread);
    }
    WSADATA winsock;
    bool winsock_ready = WSAStartup(MAKEWORD(1, 1), &winsock) == 0;
    if (mode == usb_mode::rndis) {
        if (winsock_ready && start_network(session)) {
            Sleep(INFINITE);
        }
        mode = usb_mode::sharp_serial;
    }
    for (;;) {
        wchar_t name[16] = L"";
        describe(session, transport_kind::stream, L"", serial_mode, nullptr, true);
        HANDLE port = activate_and_open(session, mode, name, 16);
        if (port == INVALID_HANDLE_VALUE) {
            append_log(L"serial port unavailable");
            set_note(session, L"Serial port unavailable. Tap Stop.");
            Sleep(INFINITE);
        }
        append_log(grimace::format<48>(L"serving %s", name).c_str());
        describe(session, transport_kind::stream, name, serial_mode, nullptr, true);
        watched_port = port;
        stream_result result = serve_stream(port, target, windows_ce_version, session.notify, session.context);
        watched_port = INVALID_HANDLE_VALUE;
        CloseHandle(port);
        if (result == stream_result::quit) {
            append_log(L"quit requested");
            return true;
        }
        if (winsock_ready && current_client_is(USB_RNDIS_CLIENT)) {
            append_log(L"USB was switched to networking; continuing as the network agent");
            if (start_network(session)) {
                Sleep(INFINITE);
            }
        }
        append_log(L"port lost, reopening");
        set_note(session, L"Connection lost, reopening...");
        Sleep(1000);
    }
}

void usb_stopped(const status_snapshot &final_state) {
    if (final_state.stop_restores_storage) {
        append_log(L"stopped, restoring mass storage");
        usb_select_client(USB_MASS_STORAGE_CLIENT);
    } else {
        append_log(L"stopped, leaving USB as it was");
    }
}

}
