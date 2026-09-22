#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
[ -x "$TEST_TMUX" ] || exit 1

if ldd "$TEST_TMUX" 2>/dev/null | grep -q libsystemd; then
	:
elif nm "$TEST_TMUX" 2>/dev/null | grep -q '[[:space:]]systemd_activated$'; then
	:
else
	echo "SKIP: $TEST_TMUX has no systemd support"
	exit 0
fi

# Whether this build carries restart support decides what a restart activation
# set is rejected with, so read it from the binary the same way as above.
if nm "$TEST_TMUX" 2>/dev/null |
    grep -q '[[:space:]]systemd_restart_get_ops$'
then
	RESTART_BUILT=1
else
	RESTART_BUILT=0
fi

exec python3 - "$TEST_TMUX" "$RESTART_BUILT" <<'PY'
import fcntl
import itertools
import os
import pty
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time


TMUX = os.path.realpath(sys.argv[1])
RESTART_BUILT = sys.argv[2] == "1"
# A build without restart support rejects a restart activation set as
# unsupported. A build with it gets as far as validating the manager-owned
# identity and rejects there instead. Both reject before accepting a client,
# which is what this asserts; only the wording differs.
UNSUPPORTED = ("systemd restart unavailable:" if RESTART_BUILT
    else "this server cannot restore a restarted server's state")
ROOT = tempfile.mkdtemp(prefix="tmux-systemd-activation.")
STRACE = shutil.which("strace")
BASE_ENV = {
    "HOME": os.environ.get("HOME", "/"),
    "LC_ALL": "C.UTF-8",
    "PATH": "/bin:/usr/bin",
    "TERM": "screen",
}
for variable in ("ASAN_OPTIONS", "LD_LIBRARY_PATH"):
    if variable in os.environ:
        BASE_ENV[variable] = os.environ[variable]
servers = []


def fail(message):
    raise RuntimeError(message)


def wait_process(pid, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found, status = os.waitpid(pid, os.WNOHANG)
        if found == pid:
            if os.WIFEXITED(status):
                return os.WEXITSTATUS(status)
            return 128 + os.WTERMSIG(status)
        time.sleep(0.01)
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    fail("timed out waiting for activation process")


def make_fds(label, kinds):
    fds = []
    paths = []
    for index, kind in enumerate(kinds):
        path = os.path.join(ROOT, f"{label}-{index}.sock")
        if kind == "listener":
            item = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            item.bind(path)
            item.listen(16)
            os.chmod(path, 0o640)
            fds.append(item.detach())
            paths.append(path)
        elif kind == "datagram":
            item = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            item.bind(path)
            fds.append(item.detach())
            paths.append(path)
        elif kind == "pty":
            master, slave = pty.openpty()
            os.close(slave)
            fds.append(master)
            paths.append(None)
        elif kind == "file":
            fds.append(os.memfd_create(label, os.MFD_CLOEXEC))
            paths.append(None)
        else:
            fail(f"unknown descriptor kind {kind}")
    return fds, paths


def child_exec(fds, names, socket_path, log_path, trace_path=None):
    log_fd = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    pid = os.fork()
    if pid != 0:
        os.close(log_fd)
        for fd in fds:
            os.close(fd)
        return pid

    saved = [fcntl.fcntl(fd, fcntl.F_DUPFD_CLOEXEC, 64) for fd in fds]
    os.dup2(log_fd, 1)
    os.dup2(log_fd, 2)
    for index, fd in enumerate(saved):
        os.dup2(fd, 3 + index, inheritable=True)
    os.closerange(3 + len(saved), 1024)

    env = BASE_ENV.copy()
    env["LISTEN_FDS"] = str(len(saved))
    if names is not None:
        env["LISTEN_FDNAMES"] = ":".join(names)
    argv = [TMUX, "-D", "-S", socket_path, "-f", "/dev/null"]
    if trace_path is None:
        env["LISTEN_PID"] = str(os.getpid())
        os.execve(TMUX, argv, env)
    shell = 'LISTEN_PID=$$; export LISTEN_PID; exec "$@"'
    os.execve(STRACE, [STRACE, "-qq", "-o", trace_path,
        "-e", "trace=close", "/bin/sh", "-c", shell, "sh", *argv], env)


def run_failure(label, entries, expected, names_override="default", trace=True):
    kinds = [kind for _, kind in entries]
    names = [name for name, _ in entries]
    if names_override != "default":
        names = names_override
    fds, paths = make_fds(label, kinds)
    socket_path = next((path for path in paths if path is not None),
        os.path.join(ROOT, f"{label}-argument.sock"))
    log_path = os.path.join(ROOT, f"{label}.log")
    trace_path = os.path.join(ROOT, f"{label}.trace") \
        if trace and STRACE is not None else None
    pid = child_exec(fds, names, socket_path, log_path, trace_path)
    status = wait_process(pid)
    with open(log_path, encoding="utf-8", errors="replace") as stream:
        output = stream.read()
    if status == 0:
        fail(f"{label}: activation unexpectedly succeeded")
    if expected not in output:
        fail(f"{label}: missing {expected!r} in {output!r}")
    if trace_path is not None:
        with open(trace_path, encoding="utf-8", errors="replace") as stream:
            closed = [int(value) for value in re.findall(
                r"close\((\d+)\)\s+= 0", stream.read())]
        for fd in range(3, 3 + len(entries)):
            if closed.count(fd) != 1:
                fail(f"{label}: descriptor {fd} closed {closed.count(fd)} times")


def client(socket_path, *args, check=True):
    result = subprocess.run([TMUX, "-S", socket_path, *args], env=BASE_ENV,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5)
    if check and result.returncode != 0:
        fail(f"client {' '.join(args)} failed: {result.stderr.strip()}")
    return result


def wait_value(socket_path, fmt, expected):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        result = client(socket_path, "display-message", "-p", fmt,
            check=False)
        if result.returncode == 0 and result.stdout.strip() == expected:
            return
        time.sleep(0.02)
    fail(f"format {fmt} did not become {expected}")


def start_server(label, name="unmanaged"):
    log_path = os.path.join(ROOT, f"{label}.log")
    socket_path = os.path.join(ROOT, f"{label}.sock")
    if name == "unmanaged":
        log = open(log_path, "w", encoding="utf-8")
        process = subprocess.Popen([TMUX, "-D", "-S", socket_path, "-f",
            "/dev/null"], env=BASE_ENV, stdout=log, stderr=log)
        log.close()
        pid = process.pid
    else:
        fds, paths = make_fds(label, ["listener"])
        socket_path = paths[0]
        names = None if name is None else [name]
        argument_path = os.path.join(ROOT, f"{label}-argument.sock")
        pid = child_exec(fds, names, argument_path, log_path)
    servers.append((pid, socket_path))
    client(socket_path, "new-session", "-d", "-s", "activation",
        "sleep 60")
    result = client(socket_path, "display-message", "-p", "#{pid}")
    if result.stdout.strip() != str(pid):
        fail(f"{label}: server PID is {result.stdout.strip()}, expected {pid}")
    return pid, socket_path


def stop_server(pid, socket_path):
    client(socket_path, "kill-server", check=False)
    wait_process(pid)
    if (pid, socket_path) in servers:
        servers.remove((pid, socket_path))


def check_activation(name, label):
    pid, socket_path = start_server(label, name)
    stop_server(pid, socket_path)


def check_unmanaged():
    pid, socket_path = start_server("unmanaged")
    if stat.S_IMODE(os.stat(socket_path).st_mode) != 0o600:
        fail("unmanaged: unexpected detached mode")
    control = subprocess.Popen([TMUX, "-S", socket_path, "-C",
        "attach-session", "-t", "activation"], env=BASE_ENV,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True)
    wait_value(socket_path, "#{session_attached}", "1")
    if stat.S_IMODE(os.stat(socket_path).st_mode) != 0o700:
        fail("unmanaged: attach did not add execute mode")
    client(socket_path, "detach-client", "-s", "activation")
    control.communicate(timeout=5)
    wait_value(socket_path, "#{session_attached}", "0")
    if stat.S_IMODE(os.stat(socket_path).st_mode) != 0o600:
        fail("unmanaged: detach did not remove execute mode")
    os.chmod(socket_path, 0o640)
    before = os.stat(socket_path)
    os.kill(pid, signal.SIGUSR1)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        after = os.stat(socket_path)
        if (after.st_dev, after.st_ino) != (before.st_dev, before.st_ino):
            break
        time.sleep(0.02)
    else:
        fail("unmanaged: SIGUSR1 did not replace socket")
    if stat.S_IMODE(after.st_mode) != 0o600:
        fail("unmanaged: replacement socket has unexpected mode")
    stop_server(pid, socket_path)


def cleanup():
    for pid, socket_path in servers[:]:
        try:
            client(socket_path, "kill-server", check=False)
        except Exception:
            pass
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
    shutil.rmtree(ROOT)


try:
    good = [
        ("tmux-server", "listener"),
        ("tmux.restart.state", "file"),
        ("tmux.p.7.123", "pty"),
        ("tmux.p.9.456", "pty"),
    ]
    for number, entries in enumerate(itertools.permutations(good)):
        run_failure(f"order-{number}", entries,
            UNSUPPORTED)
    run_failure("zero-panes", good[:2],
        UNSUPPORTED, trace=True)
    run_failure("numeric-boundaries", [good[0], good[1],
        ("tmux.p.0.1", "pty"),
        ("tmux.p.4294967295.2147483647", "pty")],
        UNSUPPORTED)
    run_failure("missing-server", good[1:], "systemd activation error:")
    run_failure("missing-state", [good[0], good[2]],
        "systemd activation error:")
    run_failure("duplicate-server", [good[0], good[0], good[1]],
        "duplicates the server socket")
    run_failure("duplicate-state", [good[0], good[1], good[1]],
        "duplicates the restart state")
    run_failure("duplicate-pane-id", [good[0], good[1], good[2],
        ("tmux.p.7.456", "pty")], "duplicates a pane ID")
    run_failure("duplicate-pane-pid", [good[0], good[1], good[2],
        ("tmux.p.9.123", "pty")], "duplicates a pane PID")
    for number, name in enumerate([
        "unknown", "stored", "connection", "",
        "tmux.p", "tmux.p.", "tmux.p.1", "tmux.p..1", "tmux.p.01.1",
        "tmux.p.1.01", "tmux.p.-1.1", "tmux.p.1.-1", "tmux.p.1.0",
        "tmux.p.4294967296.1", "tmux.p.1.2147483648", "tmux.p.1.2.extra",
        "tmux.restart.pane.1.2",
    ]):
        run_failure(f"invalid-name-{number}", [good[0], good[1],
            (name, "pty")], "has an invalid name")
    run_failure("automatic-names", good[:2], "has an invalid name",
        names_override=None)
    run_failure("name-count", good[:3], "systemd activation error:",
        names_override=[good[0][0], good[1][0]], trace=True)
    run_failure("legacy-wrong-type", [("tmux-server", "file")],
        "is not a listening Unix stream socket", trace=True)
    run_failure("restart-wrong-type", [("tmux-server", "datagram"),
        good[1]], "is not a listening Unix stream socket")
    check_activation(None, "legacy-unnamed")
    check_activation("arbitrary", "legacy-arbitrary")
    check_activation("tmux-server", "legacy-named")
    check_unmanaged()
finally:
    cleanup()
PY
