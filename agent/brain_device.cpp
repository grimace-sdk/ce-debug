#include "brain_device.h"

#include <memory>

namespace debugmgr {

namespace {

constexpr DWORD board_read_framebuffer = 0x104;
constexpr const wchar_t *board_device = L"BRD1:";
constexpr WORD bitmap_bits = 24;

struct board_framebuffer {
    DWORD width;
    DWORD height;
    DWORD bits_per_pixel;
};

uint8_t expand(uint32_t value, uint32_t maximum) {
    return static_cast<uint8_t>(value * 255 / maximum);
}

bool save_framebuffer(const std::u16string &path, uint32_t &width, uint32_t &height, uint32_t &status) {
    HANDLE board = CreateFileW(board_device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (board == INVALID_HANDLE_VALUE) {
        return false;
    }
    board_framebuffer header{};
    DWORD returned = 0;
    DeviceIoControl(board, board_read_framebuffer, nullptr, 0, &header, sizeof(header), &returned, nullptr);
    if (returned != sizeof(header) || header.bits_per_pixel != 16 || !header.width || !header.height) {
        CloseHandle(board);
        return false;
    }
    DWORD source_bytes = sizeof(header) + header.width * header.height * 2;
    DWORD row_bytes = (header.width * 3 + 3) & ~3u;
    DWORD image_bytes = row_bytes * header.height;
    std::unique_ptr<uint8_t[]> source(new uint8_t[source_bytes]);
    std::unique_ptr<uint8_t[]> image(new uint8_t[image_bytes]());
    bool read = DeviceIoControl(board, board_read_framebuffer, nullptr, 0, source.get(), source_bytes, &returned, nullptr) && returned == source_bytes;
    CloseHandle(board);
    if (!read) {
        return false;
    }
    const uint16_t *pixels = reinterpret_cast<const uint16_t *>(source.get() + sizeof(header));
    for (DWORD y = 0; y < header.height; y++) {
        const uint16_t *row = pixels + (header.height - 1 - y) * header.width;
        uint8_t *out = image.get() + y * row_bytes;
        for (DWORD x = 0; x < header.width; x++) {
            uint16_t pixel = row[x];
            out[x * 3] = expand(pixel & 0x1f, 31);
            out[x * 3 + 1] = expand(pixel >> 5 & 0x3f, 63);
            out[x * 3 + 2] = expand(pixel >> 11, 31);
        }
    }
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(header.width);
    info.biHeight = static_cast<LONG>(header.height);
    info.biPlanes = 1;
    info.biBitCount = bitmap_bits;
    info.biCompression = BI_RGB;
    info.biSizeImage = image_bytes;
    width = header.width;
    height = header.height;
    status = write_bitmap(path, info, image.get(), image_bytes);
    return true;
}

}

uint32_t brain_device::screenshot(const std::u16string &path, uint32_t &width, uint32_t &height) {
    uint32_t status = status_ok;
    if (save_framebuffer(path, width, height, status)) {
        return status;
    }
    return ce_device::screenshot(path, width, height);
}

}
