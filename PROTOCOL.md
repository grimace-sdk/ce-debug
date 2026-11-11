# debugmgr protocol

debugmgr reads requests and sends one reply to each. On CE 3.0 (`debugmgr/*.cpp`) it uses one of two transports:

- TCP, by default: it listens on port 2100 (or the port given on its command line) and serves one connection at a time. A new
  connection replaces the current one, and one with no request for 120 seconds is closed. If the port is in use, it retries every
  second. Each message is framed as a u32 little-endian length followed by the message. A request longer than the maximum message
  size closes the connection.
- An emulator's host mailbox, when the command line contains `mailbox` or the registry value `HKLM\Software\debugmgr\Transport`
  is the string `mailbox`. debugmgr calls the emulator with `swi 0x0051CE` in ARM state: r0 = operation (0 probe, 1 receive,
  2 send), r1 = buffer, r2 = length; the result is in r0, and probe also returns the maximum message size in r1. Receive returns
  0 when nothing is waiting, and debugmgr sleeps 10 ms. The emulator frames messages on its `--agent` socket as above. On real
  hardware the SWI would end debugmgr, so it's only used when selected.

On CE 1.0 and 2.0 (`debugmgr/debugmgr.c`) it uses velo-emu's host mailbox only, which delivers each message whole, framed on
its `--agent` socket as above.

All integers are little-endian. Strings are a u16 count of UTF-16 code units followed by that many UTF-16LE code units, with no
terminator. Paths are CE paths, `\` separated and absolute. Fields aren't aligned.

## Messages

Request: u16 command, u16 sequence, payload.

Reply: u16 command | 0x8000, u16 sequence (copied from the request), u32 status, payload. The payload is empty unless status is 0.

Clients use sequences 0x0001 to 0x7FFF. 0x8000 to 0xFFFF are kept for a debugger sharing the connection, such as velo-emu's GDB stub.

## Status

| Status | Meaning |
| --- | --- |
| 0 | done |
| 0x20000001 | unknown command |
| 0x20000002 | malformed request |
| 0x20000003 | no such process |
| 0x20000005 | KILL: still running after 5 seconds |
| other | the Windows error from `GetLastError`, e.g. 2 file not found, 3 path not found, 5 access denied, 80 or 183 exists, 112 disk full, 145 folder not empty |

## Commands

| # | Command | Request payload | Reply payload |
| --- | --- | --- | --- |
| 1 | PING | none | u32 protocol version (1), u32 maximum message size, u32 `_WIN32_WCE` (100, 101, 200 or 300) |
| 2 | WRITE | u32 offset, u32 flags, string path, data to the end of the message | none |
| 3 | READ | u32 offset, u32 length, string path | u32 file size, then up to length bytes from offset |
| 4 | RUN | string program, string arguments (empty for none) | u32 process ID |
| 5 | KILL | u32 process ID | none |
| 6 | LIST | string pattern, e.g. `\Windows\*` | u32 count, then per entry: u32 attributes, u32 size, string name |
| 7 | DELETE | string path | none |
| 8 | MKDIR | string path | none |
| 9 | RMDIR | string path | none |
| 10 | MOVE | string from, string to | none |
| 11 | QUIT | none | none, then debugmgr exits |
| 12 | HANDOVER | as RUN | as RUN, then debugmgr exits |
| 0x40 | SCREENSHOT | string path | u32 width, u32 height |

WRITE flag 0x1 creates the file, or truncates it if it exists. Without it the file must exist. Data is written at offset.

READ returns fewer bytes than asked at the end of the file, or when the reply would exceed the maximum message size.

The process ID is CE's `dwProcessId` from `CreateProcessW`. On CE 3.0, KILL ends any process: debugmgr keeps handles to the last 16
programs it started, and opens others with `OpenProcess`. It waits up to 5 seconds for the program to end.

On CE 1.0 and 2.0, KILL only knows the last 16 programs debugmgr started, and returns 0x20000003 for others. On CE 1.0 a process
handle reads as signalled while the program is still running, so debugmgr can't tell there whether a program has exited, and KILL
of a program that has already exited also returns 0. On CE 2.0, KILL waits up to 5 seconds for the program to end.

LIST stops early if the reply would exceed the maximum message size.

SCREENSHOT writes the screen to path as a bottom-up 24-bit BMP. Fetch it with READ and remove it with DELETE.

This is grimace's debugmgr protocol, version 1.

## GDB's file and run requests

How velo-emu's GDB stub can serve GDB's file transfer and extended-remote run through debugmgr:

| GDB | debugmgr |
| --- | --- |
| `vFile:open` with `O_CREAT` or `O_TRUNC` | WRITE at offset 0 with flag 0x1 and no data; keep the path for the descriptor |
| `vFile:open` otherwise | READ length 0 to check it exists; keep the path |
| `vFile:pwrite` | WRITE at the offset, no flags |
| `vFile:pread` | READ |
| `vFile:close` | forget the descriptor |
| `vFile:unlink` | DELETE |
| `vRun` | RUN; debug the new process and stop at its first instruction |
| `vKill` | KILL |

Windows errors map to GDB's errno values: 2 and 3 to ENOENT, 5 to EACCES, 80 and 183 to EEXIST, 112 to ENOSPC, the rest to EIO.
