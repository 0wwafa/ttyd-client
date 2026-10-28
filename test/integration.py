#!/usr/bin/env python3
"""End to end test: run ttyd-client inside a pty against a real ttyd server."""
import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time

CLIENT = os.environ.get("CLIENT", "./build/ttyd-client")
SERVER = os.environ.get("SERVER", "../ttyd-ref/build/ttyd")

passed = []
failed = []


def check(name, condition, detail=""):
    if condition:
        passed.append(name)
        print(f"  PASS  {name}")
    else:
        failed.append((name, detail))
        print(f"  FAIL  {name} {detail}")


class PtyClient:
    def __init__(self, args, cols=80, rows=24):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execv(CLIENT, [CLIENT] + args)
        self.set_size(cols, rows)
        self.buf = b""

    def set_size(self, cols, rows):
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def read(self, timeout=1.0, max_bytes=1 << 20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    return self.buf
                if not data:
                    return self.buf
                self.buf += data
                if len(self.buf) > max_bytes:
                    break
        return self.buf

    def wait_for(self, pattern, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.read(0.1)
            if re.search(pattern, self.buf.decode("utf-8", "replace")):
                return True
        return False

    def send(self, data):
        os.write(self.fd, data if isinstance(data, bytes) else data.encode())

    def wait_exit(self, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return os.waitstatus_to_exitcode(status)
            self.read(0.05)
        return None

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass


def start_server(extra_args=()):
    proc = subprocess.Popen(
        [SERVER, "-p", "0"] + list(extra_args),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    port = None
    deadline = time.time() + 10
    while time.time() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        m = re.search(r"Listening on port: (\d+)", line)
        if m:
            port = int(m.group(1))
            break
    # drain the log in the background so the pipe never fills up
    threading_drain(proc)
    return proc, port


def threading_drain(proc):
    import threading

    def drain():
        try:
            for _ in proc.stdout:
                pass
        except Exception:
            pass

    t = threading.Thread(target=drain, daemon=True)
    t.start()


def test_basic_session():
    print("\n[1] basic session: input, output, terminal title, resize")
    proc, port = start_server(["-W", "--once", "bash"])
    check("server started", port is not None)
    if port is None:
        return

    c = PtyClient(["-d", "15", f"localhost:{port}"], cols=100, rows=30)
    ok = c.wait_for(r"\$|#|>", timeout=10)
    check("shell prompt received", ok, c.buf[-300:])

    ok = c.wait_for(r"\]0;bash", timeout=2) or b"bash (" in c.buf
    check("window title set from SET_WINDOW_TITLE", ok, c.buf[:200])

    c.send("echo MARKER_$((1+1))\r")
    check("input forwarded, output received", c.wait_for(r"MARKER_2", timeout=5), c.buf[-300:])

    c.send("stty size\r")
    check("initial size honoured (30 100)", c.wait_for(r"30 100", timeout=5), c.buf[-300:])

    c.set_size(120, 40)
    time.sleep(0.4)
    c.send("stty size\r")
    check("resize forwarded (40 120)", c.wait_for(r"40 120", timeout=5), c.buf[-300:])

    c.send("exit\r")
    code = c.wait_exit(10)
    check("client exits 0 after remote exit", code == 0, f"got {code}")
    c.kill()
    proc.terminate()
    proc.wait()


def test_escape_quit():
    print("\n[2] local escape character quits without killing the shell (SIGKILL test skipped)")
    proc, port = start_server(["-W", "bash"])
    c = PtyClient([f"localhost:{port}"])
    c.wait_for(r"\$|#", timeout=10)
    c.send("\x1d")  # Ctrl-]
    code = c.wait_exit(5)
    check("ctrl-] exits the client with 0", code == 0, f"got {code}")
    c.kill()
    proc.terminate()
    proc.wait()


def test_readonly():
    print("\n[3] readonly mode does not forward input")
    proc, port = start_server(["-W", "bash"])
    c = PtyClient(["-R", f"localhost:{port}"])
    c.wait_for(r"\$|#", timeout=10)
    c.send("echo SHOULD_NOT_APPEAR\r")
    time.sleep(1.2)
    c.read(0.5)
    # the local tty echoes the keys, but the remote shell must never run them
    occurrences = c.buf.count(b"SHOULD_NOT_APPEAR")
    check("readonly ignores keyboard input", occurrences == 1, f"occurrences={occurrences} tail={c.buf[-200:]}")
    c.send("\x03")  # SIGINT -> handled as quit
    code = c.wait_exit(5)
    check("ctrl-c quits in readonly mode", code is not None, f"got {code}")
    c.kill()
    proc.terminate()
    proc.wait()


def test_auth():
    print("\n[4] basic authentication")
    proc, port = start_server(["-W", "-c", "user:pass", "--once", "bash"])
    c = PtyClient(["-c", "user:pass", f"localhost:{port}"])
    check("authenticated session works", c.wait_for(r"\$|#", timeout=10), c.buf[-300:])
    c.send("exit\r")
    c.wait_exit(10)
    c.kill()
    proc.terminate()
    proc.wait()

    proc, port = start_server(["-W", "-c", "user:pass", "bash"])
    c = PtyClient(["-c", "user:wrong", "--no-reconnect", f"localhost:{port}"])
    code = c.wait_exit(10)
    c.read(0.5)
    check("wrong credential is rejected", code not in (0, None), f"code={code} out={c.buf[-300:]}")
    c.kill()
    proc.terminate()
    proc.wait()


def test_readonly_server():
    print("\n[5] server in readonly mode (no -W)")
    proc, port = start_server(["bash"])
    c = PtyClient([f"localhost:{port}"])
    c.wait_for(r"\$|#", timeout=10)
    c.send("echo NOPE\r")
    time.sleep(0.8)
    c.read(0.5)
    check("server drops input when not writable", b"NOPE" not in c.buf, c.buf[-200:])
    c.send("\x1d")
    c.wait_exit(5)
    c.kill()
    proc.terminate()
    proc.wait()


def test_base_path_and_url_arg():
    print("\n[6] base path and url arguments")
    proc, port = start_server(["-W", "-b", "/mounted/here", "-a", "bash", "-c", 'echo "ARG:$0"'])
    c = PtyClient([f"http://localhost:{port}/mounted/here/"], cols=80, rows=24)
    check("base path url works", c.wait_for(r"ARG:bash", timeout=10), c.buf[-300:])
    c.kill()
    proc.terminate()
    proc.wait()

    proc, port = start_server(["-W", "-b", "/mounted/here", "-a", "bash", "-c", 'echo "ARG:$0"; sleep 5'])
    c = PtyClient(["-a", "hello", f"http://localhost:{port}/mounted/here/"])
    check("--url-arg forwarded", c.wait_for(r"ARG:hello", timeout=10), c.buf[-300:])
    c.kill()
    proc.terminate()
    proc.wait()


def test_tls():
    print("\n[7] TLS (wss://)")
    import tempfile

    d = tempfile.mkdtemp()
    cert = os.path.join(d, "cert.pem")
    key = os.path.join(d, "key.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert,
         "-days", "1", "-subj", "/CN=localhost",
         "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
        check=True, capture_output=True,
    )
    proc, port = start_server(["-W", "-S", "-C", cert, "-K", key, "--once", "bash"])
    if port is None:
        check("tls server started", False)
        return
    c = PtyClient(["-S", "-A", cert, f"localhost:{port}"])
    check("verified TLS session works", c.wait_for(r"\$|#", timeout=10), c.buf[-400:])
    c.send("exit\r")
    c.wait_exit(10)
    c.kill()
    proc.terminate()
    proc.wait()

    proc, port = start_server(["-W", "-S", "-C", cert, "-K", key, "--once", "bash"])
    c = PtyClient(["--insecure", f"wss://localhost:{port}/ws"])
    check("--insecure TLS session works", c.wait_for(r"\$|#", timeout=10), c.buf[-400:])
    c.send("exit\r")
    c.wait_exit(10)
    c.kill()
    proc.terminate()
    proc.wait()

    proc, port = start_server(["-W", "-S", "-C", cert, "-K", key, "bash"])
    c = PtyClient(["--no-reconnect", f"wss://localhost:{port}/ws"])
    code = c.wait_exit(10)
    c.read(0.3)
    check("self signed cert rejected by default", code not in (0, None), f"code={code} out={c.buf[-300:]}")
    c.kill()
    proc.terminate()
    proc.wait()


def test_reconnect():
    print("\n[8] reconnect behaviour")
    # a fake server that accepts and immediately closes the connection, so that
    # the websocket handshake fails: the client must retry with a backoff.
    import socket
    import threading

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(16)
    port = srv.getsockname()[1]
    stop = threading.Event()

    def acceptor():
        while not stop.is_set():
            try:
                conn, _ = srv.accept()
                conn.close()
            except OSError:
                break

    threading.Thread(target=acceptor, daemon=True).start()

    c = PtyClient(["-d", "15", "-r", "3", "--reconnect-delay", "150", f"localhost:{port}"])
    ok = c.wait_for(r"reconnecting in", timeout=10)
    check("failed handshakes trigger reconnect attempts", ok, c.buf[-300:])
    code = c.wait_exit(15)
    check("client gives up after the configured attempts", code not in (0, None), f"code={code}")
    stop.set()
    srv.close()
    c.kill()

    # the server ending the session must not trigger a reconnect
    proc, port = start_server(["-W", "--once", "bash"])
    c = PtyClient(["-d", "15", "-r", "5", f"localhost:{port}"])
    c.wait_for(r"\$|#", timeout=10)
    c.send("exit\r")
    code = c.wait_exit(10)
    check("remote exit ends the client immediately with 0", code == 0, f"code={code} tail={c.buf[-300:]}")
    c.kill()
    proc.terminate()
    proc.wait()

    # --reconnect-on-eof keeps trying like the browser client does
    proc, port = start_server(["-W", "bash"])
    c = PtyClient(["-d", "15", "--reconnect-on-eof", "-r", "2", "--reconnect-delay", "150", f"localhost:{port}"])
    c.wait_for(r"\$|#", timeout=10)
    c.send("exit\r")
    ok = c.wait_for(r"reconnecting in|connected to", timeout=10)
    check("--reconnect-on-eof reconnects after the remote exit", ok, c.buf[-300:])
    c.send("\x1d")
    c.wait_exit(5)
    c.kill()
    proc.terminate()
    proc.wait()


def test_flow_control():
    print("\n[9] flow control with a slow output sink (PAUSE/RESUME)")
    proc, port = start_server(["-W", "--once", "bash"])
    c = PtyClient(["-d", "31", "--flow-limit", "1000", "--flow-high-water", "2000", "--flow-low-water", "100",
                   f"localhost:{port}"])
    c.wait_for(r"\$|#", timeout=10)
    # generate a lot of output while not reading it, forcing the client to pause the server
    c.send("for i in $(seq 1 4000); do echo 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'; done\r")
    # do not read for a while: the pty buffer fills up and the client has to
    # pause the remote terminal instead of buffering everything in memory
    time.sleep(3.0)
    out = c.read(4.0)
    check("large output is fully delivered", out.count(b"AAAAAAAAAAAAAAAA") >= 3800, f"count={out.count(b'AAAAAAAAAAAAAAAA')}")
    check("PAUSE/RESUME flow control was used", b"sent PAUSE" in out and b"sent RESUME" in out)
    c.send("\x1d")
    c.wait_exit(5)
    c.kill()
    proc.terminate()
    proc.wait()


def test_eof_action():
    print("\n[10] stdin EOF handling in pipe mode")
    proc, port = start_server(["-W", "bash"])
    p = subprocess.run([CLIENT, "-d", "15", f"localhost:{port}"], input=b"echo PIPE_OK\rexit\r",
                       capture_output=True, timeout=20)
    check("piped stdin is forwarded", b"PIPE_OK" in p.stdout, p.stdout[-200:] + p.stderr[-300:])
    check("client exits after remote exit", p.returncode == 0, f"code={p.returncode} {p.stderr[-300:]}")
    proc.terminate()
    proc.wait()

    # -e quit must still print what the piped command produced before leaving
    proc, port = start_server(["-W", "bash"])
    p = subprocess.run([CLIENT, "-e", "quit", "--no-reconnect", f"localhost:{port}"],
                       input=b"echo QUIT_OK\r", capture_output=True, timeout=20)
    check("piped stdin with -e quit keeps the output", b"QUIT_OK" in p.stdout and p.returncode == 0,
          b"code=%d " % p.returncode + p.stdout[-200:] + p.stderr[-200:])
    proc.terminate()
    proc.wait()


if __name__ == "__main__":
    tests = [t for name, t in sorted(globals().items()) if name.startswith("test_")]
    only = sys.argv[1:] 
    for t in tests:
        if only and not any(o in t.__name__ for o in only):
            continue
        try:
            t()
        except Exception as e:
            import traceback

            traceback.print_exc()
            failed.append((t.__name__, str(e)))

    print(f"\n{len(passed)} passed, {len(failed)} failed")
    for name, detail in failed:
        print(f"  FAILED: {name} {detail}")
    sys.exit(1 if failed else 0)
