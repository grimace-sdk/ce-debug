#include "server.h"

#ifdef _WIN32_WCE
#include <winsock.h>
using socket_handle = SOCKET;
constexpr socket_handle no_socket = INVALID_SOCKET;
inline void close_socket(socket_handle handle) {
    closesocket(handle);
}
inline uint32_t milliseconds_now() {
    return GetTickCount();
}
inline void pause(uint32_t milliseconds) {
    Sleep(milliseconds);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
using socket_handle = int;
constexpr socket_handle no_socket = -1;
inline void close_socket(socket_handle handle) {
    close(handle);
}
inline uint32_t milliseconds_now() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}
inline void pause(uint32_t milliseconds) {
    usleep(milliseconds * 1000);
}
#endif

#include <memory>

namespace debugmgr {

namespace {

constexpr size_t frame_header_size = 4;
constexpr long select_timeout_milliseconds = 1000;
constexpr long send_timeout_seconds = 10;
constexpr uint32_t listen_retry_milliseconds = 1000;

struct connection {
    socket_handle socket = no_socket;
    uint32_t address = 0;
    size_t received = 0;
    size_t wanted = frame_header_size;
    bool reading_body = false;
    uint32_t last_request = 0;
    uint32_t bytes_in = 0;
};

uint32_t read_length(const uint8_t *header) {
    return header[0] | header[1] << 8 | header[2] << 16 | static_cast<uint32_t>(header[3]) << 24;
}

void write_length(uint8_t *header, size_t length) {
    for (int index = 0; index < 4; index++) {
        header[index] = static_cast<uint8_t>(length >> (index * 8));
    }
}

int select_count(socket_handle highest) {
#ifdef _WIN32_WCE
    (void)highest;
    return 0;
#else
    return highest + 1;
#endif
}

bool send_all(socket_handle connection, const uint8_t *buffer, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(connection, &writable);
        timeval timeout = {send_timeout_seconds, 0};
        if (select(select_count(connection), nullptr, &writable, nullptr, &timeout) <= 0) {
            return false;
        }
        int put = send(connection, reinterpret_cast<const char *>(buffer + sent), static_cast<int>(length - sent), 0);
        if (put <= 0) {
            return false;
        }
        sent += static_cast<size_t>(put);
    }
    return true;
}

void start_frame(connection &client) {
    client.received = 0;
    client.wanted = frame_header_size;
    client.reading_body = false;
}

void close_client(connection &client, event kind, reporter notify, void *context) {
    close_socket(client.socket);
    client.socket = no_socket;
    notify(context, {kind, client.address, 0, 0, 0, 0});
}

void adopt(connection &client, socket_handle accepted, uint32_t address, reporter notify, void *context) {
    bool replacing = client.socket != no_socket;
    if (replacing) {
        close_socket(client.socket);
    }
    int keepalive = 1;
    setsockopt(accepted, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&keepalive), sizeof(keepalive));
    client.socket = accepted;
    client.address = address;
    client.last_request = milliseconds_now();
    client.bytes_in = 0;
    start_frame(client);
    notify(context, {replacing ? event::replaced : event::connected, address, 0, 0, 0, 0});
}

enum class read_outcome { more, request_ready, closed };

read_outcome read_available(connection &client, uint8_t *header, uint8_t *request) {
    uint8_t *buffer = client.reading_body ? request : header;
    int got = recv(client.socket, reinterpret_cast<char *>(buffer + client.received), static_cast<int>(client.wanted - client.received), 0);
    if (got <= 0) {
        return read_outcome::closed;
    }
    client.received += static_cast<size_t>(got);
    client.bytes_in += static_cast<uint32_t>(got);
    if (client.received < client.wanted) {
        return read_outcome::more;
    }
    if (client.reading_body) {
        return read_outcome::request_ready;
    }
    uint32_t length = read_length(header);
    if (length > message_capacity) {
        return read_outcome::closed;
    }
    client.reading_body = true;
    client.received = 0;
    client.wanted = length;
    return length ? read_outcome::more : read_outcome::request_ready;
}

socket_handle open_listener(uint16_t port) {
    socket_handle listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == no_socket) {
        return no_socket;
    }
#ifndef _WIN32_WCE
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
#endif
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0 || listen(listener, 2) != 0) {
        close_socket(listener);
        return no_socket;
    }
    return listener;
}

}

bool serve(uint16_t port, device &target, uint32_t windows_ce_version, reporter notify, void *context, uint32_t idle_timeout) {
    std::unique_ptr<uint8_t[]> request(new uint8_t[message_capacity]);
    std::unique_ptr<uint8_t[]> frame(new uint8_t[frame_header_size + message_capacity]);
    uint8_t header[frame_header_size];
    socket_handle listener = open_listener(port);
    if (listener == no_socket) {
        notify(context, {event::listen_failed, 0, 0, 0, 0, 0});
    }
    while (listener == no_socket) {
        pause(listen_retry_milliseconds);
        listener = open_listener(port);
    }
    notify(context, {event::listening, 0, 0, 0, 0, 0});
    connection client;
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        socket_handle highest = listener;
        if (client.socket != no_socket) {
            FD_SET(client.socket, &readable);
            highest = client.socket > highest ? client.socket : highest;
        }
        timeval timeout = {select_timeout_milliseconds / 1000, select_timeout_milliseconds % 1000 * 1000};
        if (select(select_count(highest), &readable, nullptr, nullptr, &timeout) < 0) {
            continue;
        }
        if (FD_ISSET(listener, &readable)) {
            sockaddr_in remote{};
#ifdef _WIN32_WCE
            int remote_length = sizeof(remote);
#else
            socklen_t remote_length = sizeof(remote);
#endif
            socket_handle accepted = accept(listener, reinterpret_cast<sockaddr *>(&remote), &remote_length);
            if (accepted != no_socket) {
                adopt(client, accepted, ntohl(remote.sin_addr.s_addr), notify, context);
                continue;
            }
        }
        if (client.socket == no_socket) {
            continue;
        }
        if (!FD_ISSET(client.socket, &readable)) {
            if (milliseconds_now() - client.last_request > idle_timeout) {
                close_client(client, event::idle_closed, notify, context);
            }
            continue;
        }
        read_outcome outcome = read_available(client, header, request.get());
        if (outcome == read_outcome::closed) {
            close_client(client, event::disconnected, notify, context);
            continue;
        }
        if (outcome == read_outcome::more) {
            continue;
        }
        size_t length = client.wanted;
        start_frame(client);
        client.last_request = milliseconds_now();
        handled result = handle_request(request.get(), length, frame.get() + frame_header_size, message_capacity, target, windows_ce_version);
        if (result.reply_length == 0) {
            close_client(client, event::disconnected, notify, context);
            continue;
        }
        write_length(frame.get(), result.reply_length);
        size_t reply_bytes = frame_header_size + result.reply_length;
        bool sent = send_all(client.socket, frame.get(), reply_bytes);
        notify(context, {event::request, client.address, result.command, result.status, client.bytes_in,
                         sent ? static_cast<uint32_t>(reply_bytes) : 0});
        client.bytes_in = 0;
        if (!result.keep_running) {
            close_client(client, event::disconnected, notify, context);
            close_socket(listener);
            return true;
        }
        if (!sent) {
            close_client(client, event::disconnected, notify, context);
        }
    }
}

}
