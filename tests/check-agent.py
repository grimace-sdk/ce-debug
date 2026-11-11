"""
Test debugmgr's protocol and server, and client/ce-debug, end to end on this computer.

Builds tests/host/host.cpp with debugmgr's protocol and server into a program that serves a sandbox folder,
then runs each ce-debug command against it and sends some malformed requests directly.
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_PROGRAM = os.path.join(ROOT, "build", "debugmgr-host", "debugmgr-host")
TOOL = os.path.join(ROOT, "client", "ce-debug")
IDLE_MILLISECONDS = 3000


def build():
    os.makedirs(os.path.dirname(HOST_PROGRAM), exist_ok=True)
    compiler = os.environ.get("CXX", "c++")
    sources = [os.path.join(ROOT, "agent", name) for name in ("protocol.cpp", "server.cpp")] + [os.path.join(ROOT, "tests", "host", "host.cpp")]
    subprocess.run([compiler, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(ROOT, "agent")] + sources +
                   ["-o", HOST_PROGRAM], check=True)


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Checker:
    def __init__(self, port):
        self.device = "127.0.0.1:%d" % port
        self.problems = []
        self.checks = 0

    def tool(self, *arguments, expect_failure=False):
        result = subprocess.run([sys.executable, TOOL, "--device", self.device, "--timeout", "10"] + list(arguments),
                                capture_output=True, text=True)
        self.checks += 1
        if (result.returncode != 0) != expect_failure:
            self.problems.append("ce-debug %s: exit %d: %s%s" % (" ".join(arguments), result.returncode, result.stdout, result.stderr))
        return result.stdout + result.stderr

    def expect(self, condition, description):
        self.checks += 1
        if not condition:
            self.problems.append(description)


def raw_request(port, message):
    try:
        return exchange(port, message)
    except ConnectionResetError:
        return None


def exchange(port, message):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
        connection.sendall(struct.pack("<I", len(message)) + message)
        header = connection.recv(4)
        if len(header) < 4:
            return None
        length = struct.unpack("<I", header)[0]
        reply = b""
        while len(reply) < length:
            chunk = connection.recv(length - len(reply))
            if not chunk:
                break
            reply += chunk
        return reply


def run(checker, port, sandbox, work):
    output = checker.tool("ping")
    checker.expect("protocol 1, CE 3.00, 32768 byte messages" in output, "ping: %r" % output)

    payload = bytes((index * 7) % 256 for index in range(100000))
    local = os.path.join(work, "payload.bin")
    open(local, "wb").write(payload)
    checker.tool("mkdir", "/Windows")
    checker.expect(os.path.isdir(os.path.join(sandbox, "Windows")), "mkdir didn't make the folder")
    output = checker.tool("put", local, "/Windows/payload.bin")
    checker.expect("100000 bytes" in output, "put: %r" % output)
    checker.expect(open(os.path.join(sandbox, "Windows", "payload.bin"), "rb").read() == payload, "put: contents differ")

    fetched = os.path.join(work, "fetched.bin")
    output = checker.tool("get", "\\Windows\\payload.bin", fetched)
    checker.expect("100000 bytes" in output and open(fetched, "rb").read() == payload, "get: %r" % output)

    output = checker.tool("ls", "/Windows")
    checker.expect("payload.bin  100000" in output, "ls: %r" % output)
    open(os.path.join(sandbox, "Windows", "other.txt"), "w").write("x")
    output = checker.tool("ls", "\\Windows\\pay*")
    checker.expect("payload.bin  100000" in output and "other.txt" not in output, "ls with a pattern: %r" % output)
    os.remove(os.path.join(sandbox, "Windows", "other.txt"))

    checker.tool("mv", "/Windows/payload.bin", "/Windows/moved.bin")
    checker.expect(os.path.exists(os.path.join(sandbox, "Windows", "moved.bin")), "mv didn't move the file")
    output = checker.tool("get", "/Windows/payload.bin", fetched, expect_failure=True)
    checker.expect("file not found" in output, "get of a missing file: %r" % output)
    output = checker.tool("get", "/Missing/payload.bin", fetched, expect_failure=True)
    checker.expect("path not found" in output, "get from a missing folder: %r" % output)

    output = checker.tool("run", "/Windows/moved.bin", "-x", "\\Storage Card\\script.py")
    checker.expect(output.strip() == "0x00001000", "run: %r" % output)
    recorded = open(os.path.join(sandbox, "last-arguments.txt")).read()
    checker.expect(recorded == '-x "\\Storage Card\\script.py"', "run arguments: %r" % recorded)
    os.remove(os.path.join(sandbox, "last-arguments.txt"))
    checker.tool("kill", "0x1000")
    output = checker.tool("kill", "0x1000", expect_failure=True)
    checker.expect("no such process" in output, "kill of an ended process: %r" % output)
    output = checker.tool("--socket", "/nonexistent/agent.sock", "ping", expect_failure=True)
    checker.expect("ce-debug:" in output, "ping through a missing agent socket: %r" % output)

    output = checker.tool("rmdir", "/Windows", expect_failure=True)
    checker.expect("Windows error 145" in output, "rmdir of a full folder: %r" % output)
    checker.tool("rm", "/Windows/moved.bin")
    checker.tool("rmdir", "/Windows")
    checker.expect(not os.path.exists(os.path.join(sandbox, "Windows")), "rm and rmdir didn't remove the folder")

    reply = raw_request(port, struct.pack("<HH", 99, 7))
    checker.expect(reply is not None and struct.unpack_from("<HHI", reply) == (99 | 0x8000, 7, 0x20000001), "unknown command: %r" % reply)
    reply = raw_request(port, struct.pack("<HHI", 3, 8, 0))
    checker.expect(reply is not None and struct.unpack_from("<HHI", reply) == (3 | 0x8000, 8, 0x20000002), "malformed READ: %r" % reply)
    reply = raw_request(port, struct.pack("<HH", 1, 9) + b"\0" * 40000)
    checker.expect(reply is None, "an oversized request should close the connection, got %r" % reply)
    output = checker.tool("ping")
    checker.expect("protocol 1" in output, "ping after an oversized request: %r" % output)

    shot = os.path.join(work, "screen.bmp")
    output = checker.tool("screenshot", shot)
    checker.expect(output.strip() == "2x1, 62 bytes" and open(shot, "rb").read(2) == b"BM", "screenshot: %r" % output)
    checker.expect(not os.path.exists(os.path.join(sandbox, "debugmgr-screenshot.bmp")), "screenshot left its file on the device")

    first = socket.create_connection(("127.0.0.1", port), timeout=10)
    output = checker.tool("ping")
    checker.expect("protocol 1" in output, "ping while another client is connected: %r" % output)
    checker.expect(first.recv(4) == b"", "a new connection should close the previous one")
    first.close()

    idle = socket.create_connection(("127.0.0.1", port), timeout=IDLE_MILLISECONDS / 1000 + 5)
    started = time.time()
    checker.expect(idle.recv(4) == b"" and time.time() - started >= IDLE_MILLISECONDS / 1000 - 0.5,
                   "an idle connection should close after %d ms" % IDLE_MILLISECONDS)
    idle.close()

    checker.tool("quit")


def main():
    build()
    port = free_port()
    with tempfile.TemporaryDirectory() as sandbox, tempfile.TemporaryDirectory() as work:
        server = subprocess.Popen([HOST_PROGRAM, str(port), sandbox, str(IDLE_MILLISECONDS)], stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.time() + 10
            while time.time() < deadline:
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=1).close()
                    break
                except OSError:
                    time.sleep(0.05)
            checker = Checker(port)
            run(checker, port, sandbox, work)
            try:
                exit_code = server.wait(timeout=10)
                checker.expect(exit_code == 0, "debugmgr-host exited with %d after QUIT" % exit_code)
            except subprocess.TimeoutExpired:
                checker.expect(False, "debugmgr-host still running after QUIT")
        finally:
            if server.poll() is None:
                server.kill()
    for problem in checker.problems:
        print("check-agent: %s" % problem, file=sys.stderr)
    print("check-agent: %d checks, %d problems" % (checker.checks, len(checker.problems)))
    sys.exit(1 if checker.problems else 0)


if __name__ == "__main__":
    main()
