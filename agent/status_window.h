#ifndef DEBUGMGR_STATUS_WINDOW_H
#define DEBUGMGR_STATUS_WINDOW_H

#include <windows.h>

#include <grimace/cxx/fixed_string.h>
#include <grimace/cxx/gdi.h>
#include <grimace/cxx/window.h>

namespace debugmgr {

enum class transport_kind { network, mailbox, stream };

struct status_snapshot {
    transport_kind transport = transport_kind::network;
    uint16_t port = 0;
    grimace::wstring<32> stream;
    grimace::wstring<48> mode;
    grimace::wstring<128> note;
    bool stop_restores_storage = false;
    bool listening = false;
    bool failed = false;
    bool connected = false;
    uint32_t client = 0;
    DWORD connected_tick = 0;
    unsigned requests = 0;
    unsigned failures = 0;
    uint16_t last_command = 0;
    uint32_t last_status = 0;
    DWORD last_request_tick = 0;
    uint32_t bytes_in = 0;
    uint32_t bytes_out = 0;
};

struct shared_status {
    CRITICAL_SECTION lock;
    status_snapshot state;
};

constexpr UINT status_changed = WM_APP;
constexpr UINT stop_requested = WM_APP + 2;
constexpr const wchar_t *show_message_name = L"debugmgr.show";

class status_window : public grimace::window<status_window> {
public:
    static constexpr const wchar_t *class_name = L"CeDebugManager";
    static constexpr int field_count = 8;

    static HBRUSH background() {
        return GetSysColorBrush(COLOR_BTNFACE);
    }

    explicit status_window(shared_status &status) : status(status) {}

    bool open(HINSTANCE instance);
    bool on_create();
    void on_command(WORD id, WORD code, HWND control);
    bool on_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT &result);
    void on_destroy();

private:
    enum class tray_state { idle, connected, active };

    void create_fonts(int chosen_cell_height = 0);
    void shrink_fonts();
    HWND create_control(const wchar_t *control_class, const wchar_t *text, DWORD style, int id);
    int text_width(const wchar_t *text) const;
    int wrapped_height(const wchar_t *text, int width, HFONT measured_font = nullptr) const;
    int line_height() const;
    int margin() const;
    int place_group(HWND group, int first, int count, int left, int top, int width, int label_width);
    int layout(int width);
    void fit();
    status_snapshot snapshot() const;
    void refresh();
    void update_tray(const status_snapshot &now);
    void remove_tray();
    void keep_awake(const status_snapshot &now);
    void show_window();
    void show_tray_menu();
    void hide();
    void stop();

    shared_status &status;
    bool pocket = false;
    bool side_by_side = false;
    HINSTANCE instance = nullptr;
    UINT show_message = 0;
    grimace::wstring<96> addresses;
    grimace::unique_font scaled_font;
    grimace::unique_font bold_font;
    HFONT font = nullptr;
    int cell_height = 16;
    HWND explanation = nullptr;
    HWND warning = nullptr;
    HWND connection_group = nullptr;
    HWND activity_group = nullptr;
    HWND labels[field_count] = {};
    HWND values[field_count] = {};
    HWND stop_button = nullptr;
    HWND hide_button = nullptr;
    HICON tray_icons[3] = {};
    bool tray_added = false;
    tray_state shown_tray_state = tray_state::idle;
    grimace::wstring<80> shown_tip;
    DWORD last_keep_awake = 0;
    HANDLE power_off_disabled = nullptr;
};

}

#endif
