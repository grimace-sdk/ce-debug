#include <windows.h>

#include <memory>

#include "mailbox.h"

namespace debugmgr {

namespace {

constexpr int host_probe = 0;
constexpr int host_receive = 1;
constexpr int host_send = 2;
constexpr int minimum_capacity = 256;
constexpr DWORD idle_milliseconds = 10;

[[gnu::noinline]] int host_call(int operation, void *buffer, int length, int *extra) {
    register int r0 asm("r0") = operation;
    register void *r1 asm("r1") = buffer;
    register int r2 asm("r2") = length;
    asm volatile("swi 0x0051CE" : "+r"(r0), "+r"(r1) : "r"(r2) : "memory");
    if (extra) {
        *extra = static_cast<int>(reinterpret_cast<intptr_t>(r1));
    }
    return r0;
}

}

bool serve_mailbox(device &target, uint32_t windows_ce_version, reporter notify, void *context) {
    int capacity = 0;
    if (host_call(host_probe, nullptr, 0, &capacity) < 1 || capacity < minimum_capacity) {
        notify(context, {event::listen_failed, 0, 0, 0});
        return false;
    }
    std::unique_ptr<uint8_t[]> request(new uint8_t[capacity]);
    std::unique_ptr<uint8_t[]> reply(new uint8_t[capacity]);
    notify(context, {event::listening, 0, 0, 0});
    for (;;) {
        int length = host_call(host_receive, request.get(), capacity, nullptr);
        if (length <= 0) {
            Sleep(idle_milliseconds);
            continue;
        }
        handled result = handle_request(request.get(), static_cast<size_t>(length), reply.get(), static_cast<size_t>(capacity), target,
                                        windows_ce_version);
        if (result.reply_length == 0) {
            continue;
        }
        host_call(host_send, reply.get(), static_cast<int>(result.reply_length), nullptr);
        notify(context, {event::request, 0, result.command, result.status});
        if (!result.keep_running) {
            return true;
        }
    }
}

}
