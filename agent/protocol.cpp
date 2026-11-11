#include "protocol.h"

namespace debugmgr {

namespace {

constexpr size_t header_size = 4;
constexpr size_t reply_header_size = 8;
constexpr size_t path_characters = 260;
constexpr size_t argument_characters = 512;

class reader {
public:
    reader(const uint8_t *data, size_t length, size_t position) : data(data), length(length), position(position) {}

    uint16_t u16() {
        if (position + 2 > length) {
            failed = true;
            return 0;
        }
        uint16_t value = static_cast<uint16_t>(data[position] | data[position + 1] << 8);
        position += 2;
        return value;
    }

    uint32_t u32() {
        uint32_t low = u16();
        uint32_t high = u16();
        return low | high << 16;
    }

    std::u16string text(size_t maximum) {
        size_t count = u16();
        if (failed || count >= maximum || position + count * 2 > length) {
            failed = true;
            return {};
        }
        std::u16string result;
        result.reserve(count);
        for (size_t index = 0; index < count; index++) {
            result.push_back(static_cast<char16_t>(u16()));
        }
        return result;
    }

    const uint8_t *rest() const {
        return data + position;
    }

    size_t remaining() const {
        return length - position;
    }

    bool failed = false;

private:
    const uint8_t *data;
    size_t length;
    size_t position;
};

class writer {
public:
    writer(uint8_t *data, size_t capacity, size_t length) : data(data), capacity(capacity), length(length) {}

    void u16(uint16_t value) {
        if (length + 2 > capacity) {
            full = true;
            return;
        }
        data[length] = static_cast<uint8_t>(value);
        data[length + 1] = static_cast<uint8_t>(value >> 8);
        length += 2;
    }

    void u32(uint32_t value) {
        u16(static_cast<uint16_t>(value));
        u16(static_cast<uint16_t>(value >> 16));
    }

    void text(const std::u16string &value) {
        if (length + 2 + value.size() * 2 > capacity) {
            full = true;
            return;
        }
        u16(static_cast<uint16_t>(value.size()));
        for (char16_t character : value) {
            u16(static_cast<uint16_t>(character));
        }
    }

    void put_u32_at(size_t position, uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            data[position++] = static_cast<uint8_t>(value >> shift);
        }
    }

    uint8_t *data;
    size_t capacity;
    size_t length;
    bool full = false;
};

uint32_t write_file(reader &input, device &target) {
    uint32_t offset = input.u32();
    uint32_t flags = input.u32();
    std::u16string path = input.text(path_characters);
    if (input.failed) {
        return status_malformed;
    }
    return target.write_file(path, offset, flags & write_create, input.rest(), input.remaining());
}

uint32_t read_file(reader &input, writer &output, device &target) {
    uint32_t offset = input.u32();
    uint32_t length = input.u32();
    std::u16string path = input.text(path_characters);
    if (input.failed) {
        return status_malformed;
    }
    size_t size_position = output.length;
    output.u32(0);
    size_t room = output.capacity - output.length;
    uint32_t file_size = 0;
    size_t read = 0;
    uint32_t status = target.read_file(path, offset, output.data + output.length, length < room ? length : room, file_size, read);
    if (status != status_ok) {
        return status;
    }
    output.put_u32_at(size_position, file_size);
    output.length += read;
    return status_ok;
}

uint32_t run_program(reader &input, writer &output, device &target) {
    std::u16string program = input.text(path_characters);
    std::u16string arguments = input.text(argument_characters);
    if (input.failed) {
        return status_malformed;
    }
    uint32_t process = 0;
    uint32_t status = target.run(program, arguments, process);
    if (status == status_ok) {
        output.u32(process);
    }
    return status;
}

uint32_t list_folder(reader &input, writer &output, device &target) {
    std::u16string pattern = input.text(path_characters);
    if (input.failed) {
        return status_malformed;
    }
    std::vector<directory_entry> entries;
    uint32_t status = target.list(pattern, entries);
    if (status != status_ok) {
        return status;
    }
    size_t count_position = output.length;
    output.u32(0);
    uint32_t count = 0;
    for (const directory_entry &entry : entries) {
        size_t before = output.length;
        output.u32(entry.attributes);
        output.u32(entry.size);
        output.text(entry.name);
        if (output.full) {
            output.length = before;
            break;
        }
        count++;
    }
    output.put_u32_at(count_position, count);
    return status_ok;
}

uint32_t path_command(reader &input, uint16_t command, device &target) {
    std::u16string path = input.text(path_characters);
    std::u16string destination;
    if (command == command_move) {
        destination = input.text(path_characters);
    }
    if (input.failed) {
        return status_malformed;
    }
    switch (command) {
    case command_delete:
        return target.delete_file(path);
    case command_make_directory:
        return target.make_directory(path);
    case command_remove_directory:
        return target.remove_directory(path);
    default:
        return target.move(path, destination);
    }
}

}

handled handle_request(const uint8_t *request, size_t length, uint8_t *reply, size_t capacity, device &target, uint32_t windows_ce_version) {
    if (length < header_size || capacity < reply_header_size) {
        return {0, 0, status_malformed, true};
    }
    uint16_t command = static_cast<uint16_t>(request[0] | request[1] << 8);
    reader input(request, length, header_size);
    writer output(reply, capacity, reply_header_size);
    uint32_t status;
    switch (command) {
    case command_ping:
        output.u32(protocol_version);
        output.u32(static_cast<uint32_t>(capacity));
        output.u32(windows_ce_version);
        status = status_ok;
        break;
    case command_write:
        status = write_file(input, target);
        break;
    case command_read:
        status = read_file(input, output, target);
        break;
    case command_run:
    case command_handover:
        status = run_program(input, output, target);
        break;
    case command_kill: {
        uint32_t process = input.u32();
        status = input.failed ? status_malformed : target.kill(process);
        break;
    }
    case command_list:
        status = list_folder(input, output, target);
        break;
    case command_delete:
    case command_make_directory:
    case command_remove_directory:
    case command_move:
        status = path_command(input, command, target);
        break;
    case command_quit:
        status = status_ok;
        break;
    default:
        status = status_unknown_command;
        break;
    }
    if (status != status_ok) {
        output.length = reply_header_size;
    }
    uint16_t reply_command = command | reply_flag;
    reply[0] = static_cast<uint8_t>(reply_command);
    reply[1] = static_cast<uint8_t>(reply_command >> 8);
    reply[2] = request[2];
    reply[3] = request[3];
    output.put_u32_at(4, status);
    bool keep_running = command != command_quit && !(command == command_handover && status == status_ok);
    return {output.length, command, status, keep_running};
}

const char *command_name(uint16_t command) {
    switch (command) {
    case command_ping:
        return "PING";
    case command_write:
        return "WRITE";
    case command_read:
        return "READ";
    case command_run:
        return "RUN";
    case command_kill:
        return "KILL";
    case command_list:
        return "LIST";
    case command_delete:
        return "DELETE";
    case command_make_directory:
        return "MKDIR";
    case command_remove_directory:
        return "RMDIR";
    case command_move:
        return "MOVE";
    case command_quit:
        return "QUIT";
    case command_handover:
        return "HANDOVER";
    default:
        return "unknown";
    }
}

}
