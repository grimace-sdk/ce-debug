#include "stream.h"

#include <memory>

namespace debugmgr {

namespace {

constexpr size_t frame_header_size = 4;

bool read_exactly(HANDLE stream, uint8_t *buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        DWORD got = 0;
        if (!ReadFile(stream, buffer + received, static_cast<DWORD>(length - received), &got, nullptr) || got == 0) {
            return false;
        }
        received += got;
    }
    return true;
}

bool write_all(HANDLE stream, const uint8_t *buffer, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        DWORD put = 0;
        if (!WriteFile(stream, buffer + sent, static_cast<DWORD>(length - sent), &put, nullptr) || put == 0) {
            return false;
        }
        sent += put;
    }
    return true;
}

uint32_t read_length(const uint8_t *header) {
    return header[0] | header[1] << 8 | header[2] << 16 | static_cast<uint32_t>(header[3]) << 24;
}

void write_length(uint8_t *header, size_t length) {
    for (size_t index = 0; index < frame_header_size; index++) {
        header[index] = static_cast<uint8_t>(length >> (index * 8));
    }
}

}

stream_result serve_stream(HANDLE stream, device &target, uint32_t windows_ce_version, reporter notify, void *context) {
    std::unique_ptr<uint8_t[]> request(new uint8_t[message_capacity]);
    std::unique_ptr<uint8_t[]> frame(new uint8_t[frame_header_size + message_capacity]);
    uint8_t header[frame_header_size];
    notify(context, {event::connected, 0, 0, 0, 0, 0});
    stream_result outcome = stream_result::lost;
    while (read_exactly(stream, header, frame_header_size)) {
        uint32_t length = read_length(header);
        if (length > message_capacity || !read_exactly(stream, request.get(), length)) {
            break;
        }
        handled result = handle_request(request.get(), length, frame.get() + frame_header_size, message_capacity, target, windows_ce_version);
        if (result.reply_length == 0) {
            notify(context, {event::request, 0, result.command, result.status, static_cast<uint32_t>(frame_header_size + length), 0});
            continue;
        }
        write_length(frame.get(), result.reply_length);
        size_t reply_bytes = frame_header_size + result.reply_length;
        bool sent = write_all(stream, frame.get(), reply_bytes);
        notify(context, {event::request, 0, result.command, result.status, static_cast<uint32_t>(frame_header_size + length),
                         sent ? static_cast<uint32_t>(reply_bytes) : 0});
        if (!result.keep_running) {
            outcome = stream_result::quit;
            break;
        }
        if (!sent) {
            break;
        }
    }
    notify(context, {event::disconnected, 0, 0, 0, 0, 0});
    return outcome;
}

}
