#ifndef DEBUGMGR_PROTOCOL_H
#define DEBUGMGR_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace debugmgr {

constexpr uint32_t protocol_version = 1;
constexpr uint16_t default_port = 2100;
constexpr size_t message_capacity = 32768;

enum command : uint16_t {
    command_ping = 1,
    command_write = 2,
    command_read = 3,
    command_run = 4,
    command_kill = 5,
    command_list = 6,
    command_delete = 7,
    command_make_directory = 8,
    command_remove_directory = 9,
    command_move = 10,
    command_quit = 11,
    command_handover = 12,
};

constexpr uint16_t reply_flag = 0x8000;
constexpr uint32_t write_create = 0x1;

constexpr uint32_t status_ok = 0;
constexpr uint32_t status_unknown_command = 0x20000001;
constexpr uint32_t status_malformed = 0x20000002;
constexpr uint32_t status_no_such_process = 0x20000003;
constexpr uint32_t status_still_running = 0x20000005;

struct directory_entry {
    uint32_t attributes;
    uint32_t size;
    std::u16string name;
};

class device {
public:
    virtual ~device() = default;
    virtual uint32_t write_file(const std::u16string &path, uint32_t offset, bool create, const uint8_t *data, size_t length) = 0;
    virtual uint32_t read_file(const std::u16string &path, uint32_t offset, uint8_t *buffer, size_t capacity, uint32_t &file_size,
                               size_t &read) = 0;
    virtual uint32_t run(const std::u16string &program, const std::u16string &arguments, uint32_t &process) = 0;
    virtual uint32_t kill(uint32_t process) = 0;
    virtual uint32_t list(const std::u16string &pattern, std::vector<directory_entry> &entries) = 0;
    virtual uint32_t delete_file(const std::u16string &path) = 0;
    virtual uint32_t make_directory(const std::u16string &path) = 0;
    virtual uint32_t remove_directory(const std::u16string &path) = 0;
    virtual uint32_t move(const std::u16string &from, const std::u16string &to) = 0;
};

struct handled {
    size_t reply_length;
    uint16_t command;
    uint32_t status;
    bool keep_running;
};

handled handle_request(const uint8_t *request, size_t length, uint8_t *reply, size_t capacity, device &target, uint32_t windows_ce_version);

const char *command_name(uint16_t command);

}

#endif
