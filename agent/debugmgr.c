#include <windows.h>

#define PROTOCOL_VERSION 1

#define HOST_PROBE 0
#define HOST_RECEIVE 1
#define HOST_SEND 2

#define COMMAND_PING 1
#define COMMAND_WRITE 2
#define COMMAND_READ 3
#define COMMAND_RUN 4
#define COMMAND_KILL 5
#define COMMAND_LIST 6
#define COMMAND_DELETE 7
#define COMMAND_MAKE_DIRECTORY 8
#define COMMAND_REMOVE_DIRECTORY 9
#define COMMAND_MOVE 10
#define COMMAND_QUIT 11
#define COMMAND_HANDOVER 12
#define REPLY_FLAG 0x8000
#define WRITE_CREATE 0x1

#define STATUS_OK 0
#define STATUS_UNKNOWN_COMMAND 0x20000001
#define STATUS_MALFORMED 0x20000002
#define STATUS_NO_SUCH_PROCESS 0x20000003
#define STATUS_STILL_RUNNING 0x20000005

#define HEADER_SIZE 4
#define REPLY_HEADER_SIZE 8
#define MAX_PROCESSES 16
#define IDLE_MILLISECONDS 10
#define KILL_MILLISECONDS 5000
#define KILL_RETRY_MILLISECONDS 50

typedef struct {
    const BYTE *data;
    int length;
    int position;
    BOOL failed;
} Reader;

typedef struct {
    BYTE *data;
    int capacity;
    int length;
    BOOL full;
} Writer;

typedef struct {
    DWORD id;
    HANDLE handle;
} Process;

static Process processes[MAX_PROCESSES];
static int next_process;
static BYTE *request;
static BYTE *reply;
static int capacity;

#ifdef SH3
static int __attribute__((noinline)) host_call(int operation, void *buffer, int length, int *extra) {
    register int r4 asm("r4") = operation;
    register void *r5 asm("r5") = buffer;
    register int r6 asm("r6") = length;
    register int r0 asm("r0");
    register int r1 asm("r1");
    asm volatile("trapa #0xCE"
                 : "=r"(r0), "=r"(r1)
                 : "r"(r4), "r"(r5), "r"(r6)
                 : "memory");
    if (extra) {
        *extra = r1;
    }
    return r0;
}
#else
static int __attribute__((noinline)) host_call(int operation, void *buffer, int length, int *extra) {
    register int a0 asm("$4") = operation;
    register void *a1 asm("$5") = buffer;
    register int a2 asm("$6") = length;
    register int v0 asm("$2");
    register int v1 asm("$3");
    asm volatile(".set push\n.set noreorder\n.word 0x0014738d\nnop\n.set pop"
                 : "=r"(v0), "=r"(v1)
                 : "r"(a0), "r"(a1), "r"(a2)
                 : "memory");
    if (extra) {
        *extra = v1;
    }
    return v0;
}
#endif

static DWORD read_u32(Reader *reader) {
    DWORD value;
    if (reader->position + 4 > reader->length) {
        reader->failed = TRUE;
        return 0;
    }
    value = reader->data[reader->position] | (reader->data[reader->position + 1] << 8) |
            (reader->data[reader->position + 2] << 16) | ((DWORD)reader->data[reader->position + 3] << 24);
    reader->position += 4;
    return value;
}

static WORD read_u16(Reader *reader) {
    WORD value;
    if (reader->position + 2 > reader->length) {
        reader->failed = TRUE;
        return 0;
    }
    value = reader->data[reader->position] | (reader->data[reader->position + 1] << 8);
    reader->position += 2;
    return value;
}

static void read_string(Reader *reader, WCHAR *text, int text_capacity) {
    int count = read_u16(reader);
    int index;
    if (reader->failed || count >= text_capacity || reader->position + count * 2 > reader->length) {
        reader->failed = TRUE;
        text[0] = 0;
        return;
    }
    for (index = 0; index < count; index++) {
        text[index] = read_u16(reader);
    }
    text[count] = 0;
}

static void write_u16(Writer *writer, WORD value) {
    if (writer->length + 2 > writer->capacity) {
        writer->full = TRUE;
        return;
    }
    writer->data[writer->length] = value & 0xFF;
    writer->data[writer->length + 1] = value >> 8;
    writer->length += 2;
}

static void write_u32(Writer *writer, DWORD value) {
    write_u16(writer, value & 0xFFFF);
    write_u16(writer, value >> 16);
}

static void write_string(Writer *writer, const WCHAR *text) {
    int count = 0;
    int index;
    while (text[count]) {
        count++;
    }
    if (writer->length + 2 + count * 2 > writer->capacity) {
        writer->full = TRUE;
        return;
    }
    write_u16(writer, count);
    for (index = 0; index < count; index++) {
        write_u16(writer, text[index]);
    }
}

static BOOL process_ended(HANDLE handle) {
#if _WIN32_WCE >= 200
    return WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
#else
    return FALSE;
#endif
}

static DWORD last_error(void) {
    DWORD error = GetLastError();
    return error ? error : 1;
}

static DWORD write_file(Reader *reader) {
    WCHAR path[MAX_PATH];
    DWORD offset = read_u32(reader);
    DWORD flags = read_u32(reader);
    HANDLE file;
    DWORD written = 0;
    DWORD length;
    read_string(reader, path, MAX_PATH);
    if (reader->failed) {
        return STATUS_MALFORMED;
    }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, flags & WRITE_CREATE ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return last_error();
    }
    length = reader->length - reader->position;
    if (offset && SetFilePointer(file, offset, NULL, FILE_BEGIN) == 0xFFFFFFFF) {
        CloseHandle(file);
        return last_error();
    }
    if (length && (!WriteFile(file, reader->data + reader->position, length, &written, NULL) || written != length)) {
        CloseHandle(file);
        return last_error();
    }
    CloseHandle(file);
    return STATUS_OK;
}

static DWORD read_file(Reader *reader, Writer *writer) {
    WCHAR path[MAX_PATH];
    DWORD offset = read_u32(reader);
    DWORD length = read_u32(reader);
    DWORD size;
    DWORD got = 0;
    HANDLE file;
    read_string(reader, path, MAX_PATH);
    if (reader->failed) {
        return STATUS_MALFORMED;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return last_error();
    }
    size = GetFileSize(file, NULL);
    write_u32(writer, size);
    if (length > (DWORD)(writer->capacity - writer->length)) {
        length = writer->capacity - writer->length;
    }
    if (offset < size) {
        if (SetFilePointer(file, offset, NULL, FILE_BEGIN) == 0xFFFFFFFF ||
            !ReadFile(file, writer->data + writer->length, length, &got, NULL)) {
            CloseHandle(file);
            return last_error();
        }
    }
    writer->length += got;
    CloseHandle(file);
    return STATUS_OK;
}

static Process *find_process(DWORD id) {
    int index;
    for (index = 0; index < MAX_PROCESSES; index++) {
        if (processes[index].handle && processes[index].id == id) {
            return &processes[index];
        }
    }
    return NULL;
}

static DWORD run_program(Reader *reader, Writer *writer) {
    WCHAR path[MAX_PATH];
    WCHAR arguments[512];
    PROCESS_INFORMATION information;
    Process *slot = NULL;
    int index;
    read_string(reader, path, MAX_PATH);
    read_string(reader, arguments, 512);
    if (reader->failed) {
        return STATUS_MALFORMED;
    }
    for (index = 0; index < MAX_PROCESSES && !slot; index++) {
        if (!processes[index].handle) {
            slot = &processes[index];
        } else if (process_ended(processes[index].handle)) {
            CloseHandle(processes[index].handle);
            slot = &processes[index];
        }
    }
    if (!slot) {
        slot = &processes[next_process];
        next_process = (next_process + 1) % MAX_PROCESSES;
        CloseHandle(slot->handle);
    }
    if (!CreateProcessW(path, arguments[0] ? arguments : NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &information)) {
        slot->handle = NULL;
        return last_error();
    }
    CloseHandle(information.hThread);
    slot->id = information.dwProcessId;
    slot->handle = information.hProcess;
    write_u32(writer, information.dwProcessId);
    return STATUS_OK;
}

static DWORD kill_program(Reader *reader) {
    DWORD id = read_u32(reader);
    DWORD deadline;
    Process *process;
    if (reader->failed) {
        return STATUS_MALFORMED;
    }
    process = find_process(id);
    if (!process) {
        return STATUS_NO_SUCH_PROCESS;
    }
    deadline = GetTickCount() + KILL_MILLISECONDS;
    while (!process_ended(process->handle) && TerminateProcess(process->handle, 0)) {
        if ((LONG)(GetTickCount() - deadline) >= 0) {
            return STATUS_STILL_RUNNING;
        }
        Sleep(KILL_RETRY_MILLISECONDS);
    }
    CloseHandle(process->handle);
    process->handle = NULL;
    return STATUS_OK;
}

static DWORD list_folder(Reader *reader, Writer *writer) {
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    int count_position = writer->length;
    DWORD count = 0;
    read_string(reader, pattern, MAX_PATH);
    if (reader->failed) {
        return STATUS_MALFORMED;
    }
    write_u32(writer, 0);
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_NO_MORE_FILES || GetLastError() == ERROR_FILE_NOT_FOUND ? STATUS_OK : last_error();
    }
    do {
        int before = writer->length;
        write_u32(writer, found.dwFileAttributes);
        write_u32(writer, found.nFileSizeLow);
        write_string(writer, found.cFileName);
        if (writer->full) {
            writer->length = before;
            break;
        }
        count++;
    } while (FindNextFileW(search, &found));
    FindClose(search);
    writer->data[count_position] = count & 0xFF;
    writer->data[count_position + 1] = (count >> 8) & 0xFF;
    writer->data[count_position + 2] = (count >> 16) & 0xFF;
    writer->data[count_position + 3] = (count >> 24) & 0xFF;
    return STATUS_OK;
}

static DWORD path_command(Reader *reader, int command) {
    WCHAR path[MAX_PATH];
    WCHAR destination[MAX_PATH];
    BOOL done;
    read_string(reader, path, MAX_PATH);
    if (command == COMMAND_MOVE) {
        read_string(reader, destination, MAX_PATH);
    }
    if (reader->failed) {
        return STATUS_MALFORMED;
    }
    if (command == COMMAND_DELETE) {
        done = DeleteFileW(path);
    } else if (command == COMMAND_MAKE_DIRECTORY) {
        done = CreateDirectoryW(path, NULL);
    } else if (command == COMMAND_REMOVE_DIRECTORY) {
        done = RemoveDirectoryW(path);
    } else {
        done = MoveFileW(path, destination);
    }
    return done ? STATUS_OK : last_error();
}

static BOOL handle_request(int length) {
    Reader reader = { request, length, HEADER_SIZE, FALSE };
    Writer writer = { reply, capacity, REPLY_HEADER_SIZE, FALSE };
    int command = request[0] | (request[1] << 8);
    DWORD status;
    if (length < HEADER_SIZE) {
        return TRUE;
    }
    switch (command) {
    case COMMAND_PING:
        write_u32(&writer, PROTOCOL_VERSION);
        write_u32(&writer, capacity);
        write_u32(&writer, _WIN32_WCE);
        status = STATUS_OK;
        break;
    case COMMAND_WRITE:
        status = write_file(&reader);
        break;
    case COMMAND_READ:
        status = read_file(&reader, &writer);
        break;
    case COMMAND_RUN:
    case COMMAND_HANDOVER:
        status = run_program(&reader, &writer);
        break;
    case COMMAND_KILL:
        status = kill_program(&reader);
        break;
    case COMMAND_LIST:
        status = list_folder(&reader, &writer);
        break;
    case COMMAND_DELETE:
    case COMMAND_MAKE_DIRECTORY:
    case COMMAND_REMOVE_DIRECTORY:
    case COMMAND_MOVE:
        status = path_command(&reader, command);
        break;
    case COMMAND_QUIT:
        status = STATUS_OK;
        break;
    default:
        status = STATUS_UNKNOWN_COMMAND;
        break;
    }
    if (status != STATUS_OK) {
        writer.length = REPLY_HEADER_SIZE;
    }
    reply[0] = command & 0xFF;
    reply[1] = (command >> 8) | (REPLY_FLAG >> 8);
    reply[2] = request[2];
    reply[3] = request[3];
    reply[4] = status & 0xFF;
    reply[5] = (status >> 8) & 0xFF;
    reply[6] = (status >> 16) & 0xFF;
    reply[7] = (status >> 24) & 0xFF;
    host_call(HOST_SEND, reply, writer.length, NULL);
    return command != COMMAND_QUIT && !(command == COMMAND_HANDOVER && status == STATUS_OK);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show) {
    int length;
    if (host_call(HOST_PROBE, NULL, 0, &capacity) < 1 || capacity < 256) {
        return 1;
    }
    request = LocalAlloc(LMEM_FIXED, capacity);
    reply = LocalAlloc(LMEM_FIXED, capacity);
    if (!request || !reply) {
        return 1;
    }
    for (;;) {
        length = host_call(HOST_RECEIVE, request, capacity, NULL);
        if (length > 0) {
            if (!handle_request(length)) {
                break;
            }
        } else {
            Sleep(IDLE_MILLISECONDS);
        }
    }
    return 0;
}
