#include "server.h"

#ifdef _WIN32_WCE
#include <winsock.h>
using socket_handle = SOCKET;
constexpr socket_handle no_socket = INVALID_SOCKET;
inline void close_socket(socket_handle handle) {
    closesocket(handle);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle = int;
constexpr socket_handle no_socket = -1;
inline void close_socket(socket_handle handle) {
    close(handle);
}
#endif

#include <memory>

namespace debugmgr {

namespace {

constexpr size_t frame_header_size = 4;

bool receive_exactly(socket_handle connection, uint8_t *buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        int got = recv(connection, reinterpret_cast<char *>(buffer + received), static_cast<int>(length - received), 0);
        if (got <= 0) {
            return false;
        }
        received += static_cast<size_t>(got);
    }
    return true;
}

bool send_all(socket_handle connection, const uint8_t *buffer, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        int put = send(connection, reinterpret_cast<const char *>(buffer + sent), static_cast<int>(length - sent), 0);
        if (put <= 0) {
            return false;
        }
        sent += static_cast<size_t>(put);
    }
    return true;
}

uint32_t read_length(const uint8_t *header) {
    return header[0] | header[1] << 8 | header[2] << 16 | static_cast<uint32_t>(header[3]) << 24;
}

void write_length(uint8_t *header, size_t length) {
    for (int index = 0; index < 4; index++) {
        header[index] = static_cast<uint8_t>(length >> (index * 8));
    }
}

bool serve_connection(socket_handle connection, uint8_t *request, uint8_t *frame, device &target, uint32_t windows_ce_version,
                      reporter notify, void *context, uint32_t address) {
    uint8_t header[frame_header_size];
    while (receive_exactly(connection, header, frame_header_size)) {
        uint32_t length = read_length(header);
        if (length > message_capacity || !receive_exactly(connection, request, length)) {
            return true;
        }
        handled result = handle_request(request, length, frame + frame_header_size, message_capacity, target, windows_ce_version);
        if (result.reply_length == 0) {
            return true;
        }
        write_length(frame, result.reply_length);
        bool sent = send_all(connection, frame, frame_header_size + result.reply_length);
        notify(context, {event::request, address, result.command, result.status});
        if (!result.keep_running) {
            return false;
        }
        if (!sent) {
            return true;
        }
    }
    return true;
}

}

bool serve(uint16_t port, device &target, uint32_t windows_ce_version, reporter notify, void *context) {
    std::unique_ptr<uint8_t[]> request(new uint8_t[message_capacity]);
    std::unique_ptr<uint8_t[]> frame(new uint8_t[frame_header_size + message_capacity]);
    socket_handle listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == no_socket) {
        notify(context, {event::listen_failed, 0, 0, 0});
        return false;
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0 || listen(listener, 1) != 0) {
        close_socket(listener);
        notify(context, {event::listen_failed, 0, 0, 0});
        return false;
    }
    notify(context, {event::listening, 0, 0, 0});
    bool keep_running = true;
    while (keep_running) {
        sockaddr_in remote{};
#ifdef _WIN32_WCE
        int remote_length = sizeof(remote);
#else
        socklen_t remote_length = sizeof(remote);
#endif
        socket_handle connection = accept(listener, reinterpret_cast<sockaddr *>(&remote), &remote_length);
        if (connection == no_socket) {
            continue;
        }
        uint32_t address = ntohl(remote.sin_addr.s_addr);
        notify(context, {event::connected, address, 0, 0});
        keep_running = serve_connection(connection, request.get(), frame.get(), target, windows_ce_version, notify, context, address);
        close_socket(connection);
        notify(context, {event::disconnected, address, 0, 0});
    }
    close_socket(listener);
    return true;
}

}
