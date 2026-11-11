# ce-debug

Copy files to and from a Windows CE device, list folders, start and stop programs and take screenshots, from another computer.

`debugmgr.exe` runs on the device. `client/ce-debug` (Python 3) talks to it over TCP, a serial port, USB, or an emulator's host mailbox. The protocol is in [PROTOCOL.md](PROTOCOL.md).

Used on the HP Jornada 720 (CE 3.0), Dell Axim X51v (Windows Mobile 5) and Sharp Brain PW-SH2 (CE 6.0), and in velo-emu, sh3-emu and jornada-72x-emu.

## Building

Needs [grimace](https://github.com/grimace-sdk/grimace), an LLVM from [llvm-builds](https://github.com/grimace-sdk/llvm-builds/releases) (v3), CMake and Ninja.

```sh
make all GRIMACE=/path/to/grimace GRIMACE_LLVM_ROOT=/path/to/velo-llvm-v3-macos-arm64
make check
```

| Target | Output | For |
| --- | --- | --- |
| `arm-ce3` | `build/arm-ce3/agent/debugmgr.exe` | ARM CE 3.0 and later: Jornada 720, Pocket PC, Windows Mobile |
| `arm-ce6-brain` | `build/arm-ce6-brain/agent/debugmgr.exe`, `usbfn/usbserial.dll`, `usbfn/usbctl.exe` | Sharp Brain |
| `mips-ce1`, `mips-ce2` | `build/mips-ce*/agent/debugmgr.exe` | Philips Velo 1, emulator only |
| `sh3-ce2` | `build/sh3-ce2/agent/ce-debugmgr.exe` | SH3 CE 2.0, emulator only |

Below CE 3.0 debugmgr only talks to an emulator's host mailbox. On SH3 it's `ce-debugmgr.exe`, so the ROM's own `debugmgr.exe` doesn't shadow it.

`make check` builds debugmgr's protocol and server for this computer and runs every client command against it.

## Running

Copy `debugmgr.exe` to the device and start it. Its window shows how to connect, the client and recent requests. Closing it or tapping Hide leaves it running with an icon in the taskbar's tray. Starting it again brings the window back. While requests are arriving it keeps the device awake.

```sh
client/ce-debug --device 192.168.55.101 ping
client/ce-debug put build/hello.exe "/Windows/hello.exe"
client/ce-debug run "/Windows/hello.exe"
client/ce-debug ls "/Storage Card"
client/ce-debug screenshot screen.png
```

The client also has `get`, `kill`, `rm`, `mkdir`, `rmdir`, `mv`, `quit` and `handover`. `screenshot` saves a `.bmp`, or a `.png` on macOS.

Choose the device with `--device HOST[:PORT]` (or `CE_DEBUG_DEVICE`), `--serial PORT` (or `CE_DEBUG_SERIAL`), `--usb [VID:PID]` or `--socket PATH` (or `CE_DEBUG_SOCKET`) for an emulator's agent socket. With none, it uses the only `/dev/cu.usbmodem*` or `/dev/ttyACM*` port, a Sharp Brain on USB, or a Brain's USB network through brain-net on port 2100. `--wait SECONDS` waits for one to appear. `--usb` needs `pip install pyusb` and libusb.

debugmgr's transport comes from its command line, or the registry string `HKLM\Software\debugmgr\Transport`: a port number (TCP, 2100 by default), `stream=DEVICE` for a serial port, or `mailbox` in an emulator. See [PROTOCOL.md](PROTOCOL.md).

Anything that can reach debugmgr can copy files to the device and run programs, so only leave it running on a network you trust.

On Windows Mobile 5 and later, unsigned programs may show an unknown publisher prompt each time they start, and RUN or HANDOVER fail (Windows error 30123) if it isn't answered. Security policy `HKLM\Security\Policies\Policies\0000101a` set to 1 turns it off, after a soft reset.

## Sharp Brain

On the Brain debugmgr switches its USB port away from mass storage, and Stop switches it back.

- `usb` (the default): Sharp's USB serial client, `04dd:9244`. Its driver, Microsoft's `serialusbfn.dll`, is only in Sharp's updater image, so the card must supply it in `\Windows`.
- `cdc`: `usbserial.dll` from this repository, beside `debugmgr.exe`.
- `rndis`: networking over USB, for brain-net on a Mac or a Brainwave module. It needs `rndisfn.dll` in `\Windows`, which isn't included. With no connection after 2 minutes it falls back to `usb`. After a resume it switches USB back to networking.
- `auto`: `rndis` if USB networking is already on, otherwise `usb`.

macOS binds no driver to `04dd:9244`, so the client reaches it through libusb. `usbctl.exe` switches the USB function by hand: `cdc`, `msc`, or nothing for its status.

SCREENSHOT reads the framebuffer through `board.dll` (`BRD1:`) when the board driver supports it, which works while a program holds the display, and otherwise uses GDI.

## Layout

| Path | Contents |
| --- | --- |
| `agent/` | debugmgr: `debugmgr.c` for CE 1.0 and 2.0, the rest for CE 3.0 and later |
| `usbfn/` | The Brain's USB serial driver, USB function switching and `usbctl.exe` |
| `client/ce-debug` | The client |
| `assets/` | Icons |
| `tests/` | `check-agent.py` and its host build of debugmgr |

## Licence

MIT, see [LICENSE](LICENSE).
