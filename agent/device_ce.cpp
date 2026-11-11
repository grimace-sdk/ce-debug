#include "device_ce.h"

namespace debugmgr {

namespace {

constexpr DWORD kill_milliseconds = 5000;
constexpr DWORD kill_retry_milliseconds = 50;
constexpr WORD screenshot_bits = 24;
constexpr DWORD bitmap_file_header_size = 14;

static_assert(sizeof(wchar_t) == sizeof(char16_t), "CE paths are UTF-16");

const wchar_t *wide(const std::u16string &text) {
    return reinterpret_cast<const wchar_t *>(text.c_str());
}

uint32_t last_error() {
    DWORD error = GetLastError();
    return error ? error : 1;
}

bool process_ended(HANDLE handle) {
    return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

void put_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t *bytes, uint32_t value) {
    put_u16(bytes, static_cast<uint16_t>(value));
    put_u16(bytes + 2, static_cast<uint16_t>(value >> 16));
}

}

uint32_t write_bitmap(const std::u16string &path, const BITMAPINFOHEADER &info, const void *pixels, DWORD image_bytes) {
    uint8_t file_header[bitmap_file_header_size] = {'B', 'M'};
    DWORD pixel_offset = bitmap_file_header_size + sizeof(info);
    put_u32(file_header + 2, pixel_offset + image_bytes);
    put_u32(file_header + 10, pixel_offset);
    HANDLE file = CreateFileW(wide(path), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return last_error();
    }
    DWORD written = 0;
    bool ok = WriteFile(file, file_header, sizeof(file_header), &written, nullptr) && WriteFile(file, &info, sizeof(info), &written, nullptr) &&
              WriteFile(file, pixels, image_bytes, &written, nullptr) && written == image_bytes;
    uint32_t status = ok ? status_ok : last_error();
    CloseHandle(file);
    return status;
}

ce_device::~ce_device() {
    for (started_process &process : processes) {
        if (process.handle) {
            CloseHandle(process.handle);
        }
    }
}

uint32_t ce_device::write_file(const std::u16string &path, uint32_t offset, bool create, const uint8_t *data, size_t length) {
    HANDLE file = CreateFileW(wide(path), GENERIC_WRITE, 0, nullptr, create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return last_error();
    }
    DWORD written = 0;
    if ((offset && SetFilePointer(file, static_cast<LONG>(offset), nullptr, FILE_BEGIN) == 0xFFFFFFFF) ||
        (length && (!WriteFile(file, data, static_cast<DWORD>(length), &written, nullptr) || written != length))) {
        uint32_t error = last_error();
        CloseHandle(file);
        return error;
    }
    CloseHandle(file);
    return status_ok;
}

uint32_t ce_device::read_file(const std::u16string &path, uint32_t offset, uint8_t *buffer, size_t capacity, uint32_t &file_size,
                              size_t &read) {
    HANDLE file = CreateFileW(wide(path), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return last_error();
    }
    file_size = GetFileSize(file, nullptr);
    DWORD got = 0;
    if (offset < file_size && capacity) {
        if (SetFilePointer(file, static_cast<LONG>(offset), nullptr, FILE_BEGIN) == 0xFFFFFFFF ||
            !ReadFile(file, buffer, static_cast<DWORD>(capacity), &got, nullptr)) {
            uint32_t error = last_error();
            CloseHandle(file);
            return error;
        }
    }
    read = got;
    CloseHandle(file);
    return status_ok;
}

uint32_t ce_device::run(const std::u16string &program, const std::u16string &arguments, uint32_t &process) {
    started_process *slot = nullptr;
    for (started_process &candidate : processes) {
        if (!candidate.handle) {
            slot = &candidate;
            break;
        }
        if (process_ended(candidate.handle)) {
            CloseHandle(candidate.handle);
            candidate.handle = nullptr;
            slot = &candidate;
            break;
        }
    }
    if (!slot) {
        slot = &processes[next_process];
        next_process = (next_process + 1) % max_processes;
        CloseHandle(slot->handle);
        slot->handle = nullptr;
    }
    std::u16string command_line = arguments;
    PROCESS_INFORMATION information;
    if (!CreateProcessW(wide(program), command_line.empty() ? nullptr : const_cast<wchar_t *>(wide(command_line)), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, nullptr, &information)) {
        return last_error();
    }
    CloseHandle(information.hThread);
    slot->id = information.dwProcessId;
    slot->handle = information.hProcess;
    process = information.dwProcessId;
    return status_ok;
}

uint32_t ce_device::kill_other(uint32_t process) {
    HANDLE handle = OpenProcess(0, FALSE, process);
    if (!handle) {
        return status_no_such_process;
    }
    DWORD deadline = GetTickCount() + kill_milliseconds;
    while (!process_ended(handle) && TerminateProcess(handle, 0)) {
        if (static_cast<LONG>(GetTickCount() - deadline) >= 0) {
            CloseHandle(handle);
            return status_still_running;
        }
        Sleep(kill_retry_milliseconds);
    }
    CloseHandle(handle);
    return status_ok;
}

uint32_t ce_device::kill(uint32_t process) {
    started_process *found = nullptr;
    for (started_process &candidate : processes) {
        if (candidate.handle && candidate.id == process) {
            found = &candidate;
        }
    }
    if (!found) {
        return kill_other(process);
    }
    DWORD deadline = GetTickCount() + kill_milliseconds;
    while (!process_ended(found->handle) && TerminateProcess(found->handle, 0)) {
        if (static_cast<LONG>(GetTickCount() - deadline) >= 0) {
            return status_still_running;
        }
        Sleep(kill_retry_milliseconds);
    }
    CloseHandle(found->handle);
    found->handle = nullptr;
    return status_ok;
}

uint32_t ce_device::list(const std::u16string &pattern, std::vector<directory_entry> &entries) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(wide(pattern), &found);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_NO_MORE_FILES || error == ERROR_FILE_NOT_FOUND ? status_ok : last_error();
    }
    do {
        entries.push_back({found.dwFileAttributes, found.nFileSizeLow, reinterpret_cast<const char16_t *>(found.cFileName)});
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return status_ok;
}

uint32_t ce_device::delete_file(const std::u16string &path) {
    return DeleteFileW(wide(path)) ? status_ok : last_error();
}

uint32_t ce_device::make_directory(const std::u16string &path) {
    return CreateDirectoryW(wide(path), nullptr) ? status_ok : last_error();
}

uint32_t ce_device::remove_directory(const std::u16string &path) {
    return RemoveDirectoryW(wide(path)) ? status_ok : last_error();
}

uint32_t ce_device::move(const std::u16string &from, const std::u16string &to) {
    return MoveFileW(wide(from), wide(to)) ? status_ok : last_error();
}

uint32_t ce_device::screenshot(const std::u16string &path, uint32_t &width, uint32_t &height) {
    width = static_cast<uint32_t>(GetSystemMetrics(SM_CXSCREEN));
    height = static_cast<uint32_t>(GetSystemMetrics(SM_CYSCREEN));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = screenshot_bits;
    info.bmiHeader.biCompression = BI_RGB;
    DWORD image_bytes = (width * 3 + 3) / 4 * 4 * height;
    info.bmiHeader.biSizeImage = image_bytes;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    void *pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    uint32_t status = ERROR_OUTOFMEMORY;
    if (bitmap && pixels) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        BitBlt(memory, 0, 0, static_cast<int>(width), static_cast<int>(height), screen, 0, 0, SRCCOPY);
        SelectObject(memory, previous);
        status = write_bitmap(path, info.bmiHeader, pixels, image_bytes);
    }
    if (bitmap) {
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return status;
}

}
