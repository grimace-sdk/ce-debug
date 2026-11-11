#ifndef DEBUGMGR_DEVICE_CE_H
#define DEBUGMGR_DEVICE_CE_H

#include <windows.h>

#include "protocol.h"

namespace debugmgr {

class ce_device : public device {
public:
    ~ce_device() override;
    uint32_t write_file(const std::u16string &path, uint32_t offset, bool create, const uint8_t *data, size_t length) override;
    uint32_t read_file(const std::u16string &path, uint32_t offset, uint8_t *buffer, size_t capacity, uint32_t &file_size,
                       size_t &read) override;
    uint32_t run(const std::u16string &program, const std::u16string &arguments, uint32_t &process) override;
    uint32_t kill(uint32_t process) override;
    uint32_t list(const std::u16string &pattern, std::vector<directory_entry> &entries) override;
    uint32_t delete_file(const std::u16string &path) override;
    uint32_t make_directory(const std::u16string &path) override;
    uint32_t remove_directory(const std::u16string &path) override;
    uint32_t move(const std::u16string &from, const std::u16string &to) override;

private:
    uint32_t kill_other(uint32_t process);

    struct started_process {
        DWORD id = 0;
        HANDLE handle = nullptr;
    };

    static constexpr int max_processes = 16;
    started_process processes[max_processes];
    int next_process = 0;
};

}

#endif
