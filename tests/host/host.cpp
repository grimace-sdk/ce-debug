#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <set>
#include <string>

#include "protocol.h"
#include "server.h"

namespace {

constexpr uint32_t error_file_not_found = 2;
constexpr uint32_t error_path_not_found = 3;
constexpr uint32_t error_access_denied = 5;
constexpr uint32_t error_disk_full = 112;
constexpr uint32_t error_dir_not_empty = 145;
constexpr uint32_t error_already_exists = 183;
constexpr uint32_t file_attribute_directory = 0x10;
constexpr uint32_t file_attribute_normal = 0x80;
constexpr uint32_t first_process = 0x1000;
constexpr const char *last_arguments_name = "last-arguments.txt";

std::string utf8(const std::u16string &text) {
    std::string result;
    for (char16_t character : text) {
        if (character < 0x80) {
            result.push_back(static_cast<char>(character));
        } else if (character < 0x800) {
            result.push_back(static_cast<char>(0xC0 | character >> 6));
            result.push_back(static_cast<char>(0x80 | (character & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xE0 | character >> 12));
            result.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (character & 0x3F)));
        }
    }
    return result;
}

std::u16string utf16(const std::string &text) {
    std::u16string result;
    for (size_t index = 0; index < text.size(); index++) {
        unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80) {
            result.push_back(lead);
        } else if ((lead & 0xE0) == 0xC0 && index + 1 < text.size()) {
            result.push_back(static_cast<char16_t>((lead & 0x1F) << 6 | (text[index + 1] & 0x3F)));
            index++;
        } else if (index + 2 < text.size()) {
            result.push_back(static_cast<char16_t>((lead & 0x0F) << 12 | (text[index + 1] & 0x3F) << 6 | (text[index + 2] & 0x3F)));
            index += 2;
        }
    }
    return result;
}

uint32_t windows_error(int error, const std::filesystem::path &path) {
    switch (error) {
    case ENOENT:
        return std::filesystem::exists(path.parent_path()) ? error_file_not_found : error_path_not_found;
    case EACCES:
    case EPERM:
        return error_access_denied;
    case EEXIST:
        return error_already_exists;
    case ENOTEMPTY:
        return error_dir_not_empty;
    case ENOSPC:
        return error_disk_full;
    default:
        return error_access_denied;
    }
}

class sandbox_device : public debugmgr::device {
public:
    explicit sandbox_device(std::filesystem::path root) : root(std::move(root)) {}

    uint32_t write_file(const std::u16string &path, uint32_t offset, bool create, const uint8_t *data, size_t length) override {
        std::filesystem::path local = map(path);
        FILE *file = fopen(local.c_str(), create ? "wb" : "r+b");
        if (!file) {
            return windows_error(errno, local);
        }
        bool written = fseek(file, offset, SEEK_SET) == 0 && fwrite(data, 1, length, file) == length;
        fclose(file);
        return written ? debugmgr::status_ok : error_disk_full;
    }

    uint32_t read_file(const std::u16string &path, uint32_t offset, uint8_t *buffer, size_t capacity, uint32_t &file_size,
                       size_t &read) override {
        std::filesystem::path local = map(path);
        FILE *file = fopen(local.c_str(), "rb");
        if (!file) {
            return windows_error(errno, local);
        }
        fseek(file, 0, SEEK_END);
        file_size = static_cast<uint32_t>(ftell(file));
        read = 0;
        if (offset < file_size && fseek(file, offset, SEEK_SET) == 0) {
            read = fread(buffer, 1, capacity, file);
        }
        fclose(file);
        return debugmgr::status_ok;
    }

    uint32_t run(const std::u16string &program, const std::u16string &arguments, uint32_t &process) override {
        std::filesystem::path local = map(program);
        if (!std::filesystem::is_regular_file(local)) {
            return windows_error(ENOENT, local);
        }
        FILE *record = fopen((root / last_arguments_name).c_str(), "wb");
        if (record) {
            std::string text = utf8(arguments);
            fwrite(text.data(), 1, text.size(), record);
            fclose(record);
        }
        process = next_process++;
        running.insert(process);
        return debugmgr::status_ok;
    }

    uint32_t kill(uint32_t process) override {
        return running.erase(process) ? debugmgr::status_ok : debugmgr::status_no_such_process;
    }

    uint32_t list(const std::u16string &pattern, std::vector<debugmgr::directory_entry> &entries) override {
        std::string text = utf8(pattern);
        size_t separator = text.find_last_of('\\');
        std::string folder = separator == std::string::npos ? "" : text.substr(0, separator);
        std::string wildcard = separator == std::string::npos ? text : text.substr(separator + 1);
        std::filesystem::path local = map(utf16(folder));
        std::error_code error;
        std::filesystem::directory_iterator iterator(local, error);
        if (error) {
            return windows_error(error.value(), local / "x");
        }
        for (const auto &item : iterator) {
            std::string name = item.path().filename().string();
            if (fnmatch(wildcard.c_str(), name.c_str(), FNM_CASEFOLD) != 0) {
                continue;
            }
            bool directory = item.is_directory();
            entries.push_back({directory ? file_attribute_directory : file_attribute_normal,
                               directory ? 0 : static_cast<uint32_t>(item.file_size()), utf16(name)});
        }
        return debugmgr::status_ok;
    }

    uint32_t delete_file(const std::u16string &path) override {
        std::filesystem::path local = map(path);
        return unlink(local.c_str()) == 0 ? debugmgr::status_ok : windows_error(errno, local);
    }

    uint32_t make_directory(const std::u16string &path) override {
        std::filesystem::path local = map(path);
        return mkdir(local.c_str(), 0755) == 0 ? debugmgr::status_ok : windows_error(errno, local);
    }

    uint32_t remove_directory(const std::u16string &path) override {
        std::filesystem::path local = map(path);
        return rmdir(local.c_str()) == 0 ? debugmgr::status_ok : windows_error(errno, local);
    }

    uint32_t move(const std::u16string &from, const std::u16string &to) override {
        std::filesystem::path source = map(from);
        std::filesystem::path destination = map(to);
        if (std::filesystem::exists(destination)) {
            return error_already_exists;
        }
        return rename(source.c_str(), destination.c_str()) == 0 ? debugmgr::status_ok : windows_error(errno, source);
    }

private:
    std::filesystem::path map(const std::u16string &path) const {
        std::string text = utf8(path);
        std::filesystem::path local = root;
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find('\\', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            std::string part = text.substr(start, end - start);
            if (!part.empty() && part != "." && part != "..") {
                local /= part;
            }
            start = end + 1;
        }
        return local;
    }

    std::filesystem::path root;
    uint32_t next_process = first_process;
    std::set<uint32_t> running;
};

void report(void *, const debugmgr::report &happened) {
    if (happened.kind == debugmgr::event::listening) {
        fprintf(stderr, "debugmgr-host: listening\n");
    } else if (happened.kind == debugmgr::event::listen_failed) {
        fprintf(stderr, "debugmgr-host: can't listen\n");
    }
}

}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: debugmgr-host PORT ROOT\n");
        return 2;
    }
    sandbox_device device{std::filesystem::path(argv[2])};
    return debugmgr::serve(static_cast<uint16_t>(atoi(argv[1])), device, 300, report, nullptr) ? 0 : 1;
}
