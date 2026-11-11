#include "status_window.h"

#include <shellapi.h>
#include <winsock.h>

#include <grimace/cxx/format.h>

#include "protocol.h"

namespace debugmgr {

namespace {

constexpr int stop_button_id = 1;
constexpr int hide_button_id = 2;
constexpr int tray_open_command = 11;
constexpr int tray_stop_command = 12;
constexpr UINT tray_icon_id = 1;
constexpr UINT tray_message = WM_APP + 1;
constexpr int tray_icon_size = 16;
constexpr int program_icon_resource = 1;
constexpr int tray_icon_resources[] = {2, 3, 4};
constexpr UINT_PTR refresh_timer = 1;
constexpr UINT refresh_milliseconds = 1000;
constexpr UINT_PTR address_timer = 2;
constexpr UINT address_refresh_milliseconds = 3000;
constexpr DWORD active_milliseconds = 2000;
constexpr DWORD in_use_milliseconds = 60000;
constexpr DWORD keep_awake_milliseconds = 10000;
constexpr int minimum_width_in_cells = 30;
constexpr int readable_fraction_of_screen = 28;
constexpr int smallest_cell_height = 10;

constexpr const wchar_t *field_labels[status_window::field_count] = {
    L"Mode:", L"Address:", L"Client:", L"Connected for:", L"Last request:", L"Requests:", L"Received:", L"Sent:",
};

constexpr const wchar_t *power_events[] = {
    L"PowerManager/ActivityTimer/EdUserActivity",
    L"PowerManager/ActivityTimer/EdSystemActivity",
    L"PowerManager/EdPowerTimerReset",
};

constexpr DWORD empty_menu_bar = 0x0001;

struct menu_bar_info {
    DWORD size;
    HWND parent;
    DWORD flags;
    UINT toolbar;
    HINSTANCE resources;
    int bitmap;
    int bitmap_count;
    HWND bar;
    COLORREF background;
};

using create_menu_bar_function = BOOL(WINAPI *)(menu_bar_info *);

static_assert(sizeof(NOTIFYICONDATAW) == 152, "Windows CE's NOTIFYICONDATAW has a 64 character tip and nothing after it");

grimace::wstring<48> dotted(uint32_t address) {
    return grimace::format<48>(L"%u.%u.%u.%u", address >> 24, (address >> 16) & 0xFF, (address >> 8) & 0xFF, address & 0xFF);
}

grimace::wstring<96> local_addresses(uint16_t port) {
    grimace::wstring<96> result;
    char name[64];
    if (gethostname(name, sizeof(name)) != 0) {
        return result;
    }
    hostent *host = gethostbyname(name);
    if (!host) {
        return result;
    }
    for (int index = 0; host->h_addr_list[index]; index++) {
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(host->h_addr_list[index]);
        uint32_t address = static_cast<uint32_t>(bytes[0]) << 24 | bytes[1] << 16 | bytes[2] << 8 | bytes[3];
        if (!result.empty()) {
            result.append(L", ");
        }
        result.append(grimace::format<64>(L"%s:%u", dotted(address), static_cast<unsigned>(port)));
    }
    return result;
}

grimace::wstring<32> duration(DWORD milliseconds) {
    DWORD seconds = milliseconds / 1000;
    if (seconds < 60) {
        return grimace::format<32>(L"%u s", static_cast<unsigned>(seconds));
    }
    if (seconds < 3600) {
        return grimace::format<32>(L"%u min %02u s", static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
    }
    return grimace::format<32>(L"%u h %02u min", static_cast<unsigned>(seconds / 3600), static_cast<unsigned>(seconds / 60 % 60));
}

grimace::wstring<32> byte_count(uint32_t bytes) {
    if (bytes < 1024) {
        return grimace::format<32>(L"%u bytes", bytes);
    }
    if (bytes < 1024 * 1024) {
        return grimace::format<32>(L"%u.%u KB", bytes / 1024, bytes % 1024 * 10 / 1024);
    }
    return grimace::format<32>(L"%u.%u MB", bytes / (1024 * 1024), bytes % (1024 * 1024) / 1024 * 10 / 1024);
}

void set_text_if_changed(HWND control, const wchar_t *text) {
    wchar_t current[192];
    GetWindowTextW(control, current, 192);
    if (lstrcmpW(current, text)) {
        SetWindowTextW(control, text);
    }
}

int menu_bar_top(HWND window) {
    HMODULE shell = LoadLibraryW(L"aygshell.dll");
    if (!shell) {
        return -1;
    }
    auto create_menu_bar = reinterpret_cast<create_menu_bar_function>(GetProcAddressW(shell, L"SHCreateMenuBar"));
    menu_bar_info info{};
    info.size = sizeof(info);
    info.parent = window;
    info.flags = empty_menu_bar;
    if (!create_menu_bar || !create_menu_bar(&info) || !info.bar) {
        FreeLibrary(shell);
        return -1;
    }
    RECT bar;
    GetWindowRect(info.bar, &bar);
    return bar.top;
}

int font_cell_height(HFONT chosen) {
    grimace::window_dc dc(nullptr);
    auto selected = dc.select(chosen);
    return dc.text_height();
}

}

bool status_window::open(HINSTANCE program) {
    instance = program;
    show_message = RegisterWindowMessageW(show_message_name);
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    pocket = screen_width < screen_height;
    create_fonts();
    RECT bounds = grimace::work_area_rect();
    DWORD style = WS_CLIPCHILDREN;
    if (!pocket) {
        int width = screen_width * 3 / 5;
        int minimum = cell_height * minimum_width_in_cells;
        width = width < minimum ? (minimum < screen_width ? minimum : screen_width) : width;
        bounds = RECT{0, 0, width, 100};
        style |= WS_POPUP | WS_CAPTION | WS_SYSMENU;
    }
    if (!register_class(instance) || !create(instance, L"debugmgr", style, bounds)) {
        return false;
    }
    HICON small_icon =
        static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(program_icon_resource), IMAGE_ICON, tray_icon_size, tray_icon_size, 0));
    SendMessageW(handle(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    if (pocket) {
        int bottom = menu_bar_top(handle());
        if (bottom > bounds.top && bottom < bounds.bottom) {
            MoveWindow(handle(), bounds.left, bounds.top, bounds.right - bounds.left, bottom - bounds.top, FALSE);
        }
    }
    refresh();
    fit();
    show(SW_SHOW);
    UpdateWindow(handle());
    return true;
}

bool status_window::on_create() {
    explanation = create_control(L"STATIC", L"Starting...", SS_LEFT | SS_NOPREFIX, 0);
    SendMessageW(explanation, WM_SETFONT, reinterpret_cast<WPARAM>(bold_font.get()), FALSE);
    warning = create_control(L"STATIC",
                             mailbox ? L"Requests come from the emulator."
                                     : L"Anything that can reach this port can copy files to this device and run programs.",
                             SS_LEFT | SS_NOPREFIX, 0);
    connection_group = create_control(L"BUTTON", L"Connection", BS_GROUPBOX, 0);
    activity_group = create_control(L"BUTTON", L"Activity", BS_GROUPBOX, 0);
    for (int index = 0; index < field_count; index++) {
        labels[index] = create_control(L"STATIC", field_labels[index], SS_LEFT | SS_NOPREFIX, 0);
        values[index] = create_control(L"STATIC", L"-", SS_LEFT | SS_NOPREFIX, 0);
    }
    stop_button = create_control(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, stop_button_id);
    hide_button = create_control(L"BUTTON", L"Hide", BS_PUSHBUTTON | WS_TABSTOP, hide_button_id);
    SetWindowPos(connection_group, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(activity_group, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    addresses = local_addresses(port);
    SetTimer(handle(), refresh_timer, refresh_milliseconds, nullptr);
    SetTimer(handle(), address_timer, address_refresh_milliseconds, nullptr);
    return true;
}

void status_window::on_command(WORD id, WORD, HWND) {
    if (id == stop_button_id) {
        stop();
    } else if (id == hide_button_id) {
        hide();
    }
}

bool status_window::on_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT &result) {
    result = 0;
    if (show_message && message == show_message) {
        show_window();
        return true;
    }
    switch (message) {
    case status_changed:
        refresh();
        return true;
    case stop_requested:
        stop();
        return true;
    case WM_TIMER:
        if (wparam == refresh_timer) {
            refresh();
            return true;
        }
        if (wparam == address_timer) {
            grimace::wstring<96> refreshed = local_addresses(port);
            if (refreshed.view() != addresses.view()) {
                addresses = refreshed;
                refresh();
                fit();
            }
            return true;
        }
        return false;
    case WM_CLOSE:
        hide();
        return true;
    case tray_message:
        if (lparam == WM_LBUTTONUP) {
            if (IsWindowVisible(handle())) {
                show_tray_menu();
            } else {
                show_window();
            }
        }
        return true;
    }
    return false;
}

void status_window::on_destroy() {
    KillTimer(handle(), refresh_timer);
    KillTimer(handle(), address_timer);
    remove_tray();
    PostQuitMessage(0);
}

void status_window::create_fonts(int chosen_cell_height) {
    HFONT system_font = static_cast<HFONT>(GetStockObject(SYSTEM_FONT));
    LOGFONTW description{};
    GetObjectW(system_font, sizeof(description), &description);
    int screen_width = GetSystemMetrics(SM_CXSCREEN);
    int screen_height = GetSystemMetrics(SM_CYSCREEN);
    int readable = (screen_width < screen_height ? screen_width : screen_height) / readable_fraction_of_screen;
    font = system_font;
    if (chosen_cell_height) {
        description.lfHeight = chosen_cell_height;
        description.lfWidth = 0;
        scaled_font = grimace::create_font(description);
        font = scaled_font.get();
    } else if (font_cell_height(system_font) < readable) {
        description.lfHeight = -readable * 4 / 5;
        description.lfWidth = 0;
        scaled_font = grimace::create_font(description);
        font = scaled_font.get();
    }
    description.lfWeight = FW_BOLD;
    bold_font = grimace::create_font(description);
    cell_height = font_cell_height(font);
}

HWND status_window::create_control(const wchar_t *control_class, const wchar_t *text, DWORD style, int id) {
    HWND control = CreateWindowExW(0, control_class, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, handle(),
                                   reinterpret_cast<HMENU>(id), instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return control;
}

int status_window::text_width(const wchar_t *text) const {
    grimace::window_dc dc(handle());
    auto selected = dc.select(font);
    RECT measured{};
    DrawTextW(dc.get(), text, -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    return measured.right - measured.left;
}

int status_window::wrapped_height(const wchar_t *text, int width, HFONT measured_font) const {
    grimace::window_dc dc(handle());
    auto selected = dc.select(measured_font ? measured_font : font);
    RECT measured{0, 0, width - cell_height / 2, 0};
    DrawTextW(dc.get(), text, -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    return measured.bottom - measured.top;
}

int status_window::line_height() const {
    return cell_height + cell_height / 3;
}

int status_window::margin() const {
    return cell_height / 2;
}

int status_window::place_group(HWND group, int first, int count, int left, int top, int width, int label_width) {
    int row_top = top + line_height();
    int value_left = left + margin() * 2 + label_width;
    int value_width = width - margin() * 4 - label_width;
    for (int index = first; index < first + count; index++) {
        wchar_t text[192];
        GetWindowTextW(values[index], text, 192);
        int height = wrapped_height(text, value_width);
        height = height > cell_height ? height + line_height() - cell_height : line_height();
        MoveWindow(labels[index], left + margin() * 2, row_top, label_width, line_height(), TRUE);
        MoveWindow(values[index], value_left, row_top, value_width, height, TRUE);
        row_top += height;
    }
    int bottom = row_top + margin();
    MoveWindow(group, left, top, width, bottom - top, TRUE);
    return bottom;
}

int status_window::layout(int width) {
    int gap = margin();
    int inner = width - gap * 2;
    int top = gap;
    wchar_t text[192];
    GetWindowTextW(explanation, text, 192);
    int explanation_height = wrapped_height(text, inner, bold_font.get()) + cell_height / 4;
    MoveWindow(explanation, gap, top, inner, explanation_height, TRUE);
    top += explanation_height;
    GetWindowTextW(warning, text, 192);
    int warning_height = wrapped_height(text, inner);
    MoveWindow(warning, gap, top, inner, warning_height, TRUE);
    top += warning_height + gap;
    int label_width = 0;
    for (const wchar_t *label : field_labels) {
        int measured = text_width(label);
        label_width = measured > label_width ? measured : label_width;
    }
    label_width += gap;
    if (side_by_side) {
        int half = (inner - gap) / 2;
        int connection_bottom = place_group(connection_group, 0, 4, gap, top, half, label_width);
        int activity_bottom = place_group(activity_group, 4, 4, gap * 2 + half, top, inner - half - gap, label_width);
        top = (connection_bottom > activity_bottom ? connection_bottom : activity_bottom) + gap;
    } else {
        top = place_group(connection_group, 0, 4, gap, top, inner, label_width) + gap / 2;
        top = place_group(activity_group, 4, 4, gap, top, inner, label_width) + gap;
    }
    int button_height = cell_height + cell_height * 3 / 4;
    if (pocket) {
        int bottom = client_rect().bottom - gap - button_height;
        top = bottom > top ? bottom : top;
    }
    int stop_width = text_width(L"Stop") + gap * 6;
    int hide_width = text_width(L"Hide") + gap * 6;
    MoveWindow(stop_button, gap, top, stop_width, button_height, TRUE);
    MoveWindow(hide_button, width - gap - hide_width, top, hide_width, button_height, TRUE);
    return top + button_height + gap;
}

void status_window::shrink_fonts() {
    create_fonts(cell_height - 1);
    SendMessageW(explanation, WM_SETFONT, reinterpret_cast<WPARAM>(bold_font.get()), FALSE);
    for (HWND control : {warning, connection_group, activity_group, stop_button, hide_button}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
    for (int index = 0; index < field_count; index++) {
        SendMessageW(labels[index], WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        SendMessageW(values[index], WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
}

void status_window::fit() {
    RECT client = client_rect();
    int content_height = layout(client.right);
    if (pocket) {
        while (content_height > client.bottom && cell_height > smallest_cell_height) {
            shrink_fonts();
            content_height = layout(client.right);
        }
        InvalidateRect(handle(), nullptr, TRUE);
        return;
    }
    RECT area = grimace::work_area_rect();
    RECT frame{0, 0, client.right, content_height};
    AdjustWindowRectEx(&frame, GetWindowLongW(handle(), GWL_STYLE), FALSE, GetWindowLongW(handle(), GWL_EXSTYLE));
    if (!side_by_side && frame.bottom - frame.top > area.bottom - area.top) {
        side_by_side = true;
        int client_width = area.right - area.left - (frame.right - frame.left - client.right);
        content_height = layout(client_width);
        frame = RECT{0, 0, client_width, content_height};
        AdjustWindowRectEx(&frame, GetWindowLongW(handle(), GWL_STYLE), FALSE, GetWindowLongW(handle(), GWL_EXSTYLE));
    }
    int frame_width = frame.right - frame.left;
    int frame_height = frame.bottom - frame.top;
    int left = area.left + (area.right - area.left - frame_width) / 2;
    int top = area.top + (area.bottom - area.top - frame_height) / 3;
    MoveWindow(handle(), left, top < area.top ? area.top : top, frame_width, frame_height, TRUE);
}

status_snapshot status_window::snapshot() const {
    EnterCriticalSection(&status.lock);
    status_snapshot now = status.state;
    LeaveCriticalSection(&status.lock);
    return now;
}

void status_window::refresh() {
    status_snapshot now = snapshot();
    DWORD tick = GetTickCount();
    grimace::wstring<160> headline;
    if (mailbox) {
        headline = now.failed ? grimace::wstring<160>(L"No emulator host mailbox: run on the emulator, or without \"mailbox\".")
                              : grimace::wstring<160>(L"Serving the emulator's host mailbox.");
    } else if (now.failed) {
        headline = grimace::format<160>(L"Port %u is in use. Waiting for it...", static_cast<unsigned>(port));
    } else if (now.connected) {
        headline = grimace::format<160>(L"%s is connected.", dotted(now.client));
    } else if (addresses.empty()) {
        headline = L"Waiting for a network address.";
    } else {
        headline = L"Ready. Connect with ce-debug.";
    }
    set_text_if_changed(explanation, headline.c_str());

    grimace::wstring<128> fields[field_count];
    fields[0] = mailbox ? L"Emulator mailbox" : L"Network";
    if (mailbox) {
        fields[1] = L"-";
    } else if (addresses.empty()) {
        fields[1] = grimace::format<128>(L"Port %u, no address yet", static_cast<unsigned>(port));
    } else {
        fields[1] = addresses.c_str();
    }
    fields[2] = mailbox ? grimace::wstring<128>(L"Emulator") : now.connected ? grimace::wstring<128>(dotted(now.client).c_str()) : grimace::wstring<128>(L"None");
    fields[3] = now.connected ? grimace::wstring<128>(duration(tick - now.connected_tick).c_str()) : grimace::wstring<128>(L"-");
    if (now.requests) {
        fields[4] = now.last_status == status_ok
                        ? grimace::format<128>(L"%S, ok, %s ago", command_name(now.last_command), duration(tick - now.last_request_tick))
                        : grimace::format<128>(L"%S, error %X, %s ago", command_name(now.last_command), now.last_status,
                                              duration(tick - now.last_request_tick));
    } else {
        fields[4] = L"None yet";
    }
    fields[5] = now.failures ? grimace::format<128>(L"%u, %u failed", now.requests, now.failures) : grimace::format<128>(L"%u", now.requests);
    fields[6] = byte_count(now.bytes_in).c_str();
    fields[7] = byte_count(now.bytes_out).c_str();
    for (int index = 0; index < field_count; index++) {
        set_text_if_changed(values[index], fields[index].c_str());
    }
    update_tray(now);
    keep_awake(now);
}

void status_window::update_tray(const status_snapshot &now) {
    DWORD tick = GetTickCount();
    tray_state state = tray_state::idle;
    if (now.connected || (mailbox && now.listening)) {
        state = now.requests && tick - now.last_request_tick < active_milliseconds ? tray_state::active : tray_state::connected;
    }
    grimace::wstring<80> tip = now.connected ? grimace::format<80>(L"debugmgr: %s connected", dotted(now.client))
                                             : grimace::wstring<80>(L"debugmgr: waiting");
    if (tray_added && state == shown_tray_state && tip.view() == shown_tip.view()) {
        return;
    }
    int index = static_cast<int>(state);
    if (!tray_icons[index]) {
        tray_icons[index] = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(tray_icon_resources[index]), IMAGE_ICON, tray_icon_size, tray_icon_size, 0));
    }
    shown_tray_state = state;
    shown_tip = tip;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = handle();
    data.uID = tray_icon_id;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = tray_message;
    data.hIcon = tray_icons[index];
    lstrcpynW(data.szTip, tip.c_str(), 64);
    if (tray_added) {
        Shell_NotifyIcon(NIM_MODIFY, &data);
        return;
    }
    tray_added = Shell_NotifyIcon(NIM_ADD, &data) != FALSE;
    EnableWindow(hide_button, tray_added);
}

void status_window::remove_tray() {
    if (!tray_added) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = handle();
    data.uID = tray_icon_id;
    Shell_NotifyIcon(NIM_DELETE, &data);
    tray_added = false;
}

void status_window::keep_awake(const status_snapshot &now) {
    DWORD tick = GetTickCount();
    if (!now.requests || tick - now.last_request_tick >= in_use_milliseconds) {
        return;
    }
    if (last_keep_awake && tick - last_keep_awake < keep_awake_milliseconds) {
        return;
    }
    last_keep_awake = tick;
    SystemIdleTimerReset();
    for (const wchar_t *name : power_events) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, name);
        if (event) {
            SetEvent(event);
            CloseHandle(event);
        }
    }
}

void status_window::show_window() {
    ShowWindow(handle(), SW_SHOW);
    SetForegroundWindow(handle());
}

void status_window::show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, tray_open_command, L"Open debugmgr");
    AppendMenuW(menu, MF_STRING, tray_stop_command, L"Stop debugmgr");
    POINT point;
    GetCursorPos(&point);
    SetForegroundWindow(handle());
    UINT command = TrackPopupMenuEx(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RETURNCMD, point.x, point.y, handle(), nullptr);
    DestroyMenu(menu);
    if (command == tray_open_command) {
        show_window();
    } else if (command == tray_stop_command) {
        stop();
    }
}

void status_window::hide() {
    if (tray_added) {
        ShowWindow(handle(), SW_HIDE);
    }
}

void status_window::stop() {
    destroy();
}

}
