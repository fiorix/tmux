#!/bin/sh

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
[ -x "$TEST_TMUX" ] || exit 1

if nm "$TEST_TMUX" 2>/dev/null |
    grep -q '[[:space:]]systemd_activated$'; then
	SYSTEMD_ENABLED=1
else
	SYSTEMD_ENABLED=0
fi
if nm "$TEST_TMUX" 2>/dev/null |
    grep -q '[[:space:]]systemd_restart_get_ops$'; then
	SYSTEMD_RESTART_ENABLED=1
else
	SYSTEMD_RESTART_ENABLED=0
fi
if nm "$TEST_TMUX" 2>/dev/null |
    grep -q '[[:space:]]environ_unset_systemd$'; then
	SYSTEMD_ENVIRONMENT_CLEAN=1
else
	SYSTEMD_ENVIRONMENT_CLEAN=0
fi
[ "$SYSTEMD_RESTART_ENABLED" = 0 ] || [ "$SYSTEMD_ENABLED" = 1 ] || exit 1
export SYSTEMD_ENABLED SYSTEMD_RESTART_ENABLED SYSTEMD_ENVIRONMENT_CLEAN

exec python3 - "$TEST_TMUX" <<'PY'
import os
import pty
import shutil
import socket
import subprocess
import sys
import tempfile
import time


TMUX = os.path.realpath(sys.argv[1])
ROOT = tempfile.mkdtemp(prefix="tmux-systemd-handoff.")
BASE_ENV = {
    "HOME": ROOT,
    "LC_ALL": "C.UTF-8",
    "PATH": "/bin:/usr/bin",
    "TERM": "screen",
}
for variable in ("ASAN_OPTIONS", "LD_LIBRARY_PATH", "UBSAN_OPTIONS"):
    if variable in os.environ:
        BASE_ENV[variable] = os.environ[variable]
SYSTEMD_VARIABLES = (
    "LISTEN_PID",
    "LISTEN_PIDFDID",
    "LISTEN_FDS",
    "LISTEN_FDNAMES",
    "NOTIFY_SOCKET",
    "FDSTORE",
    "WATCHDOG_PID",
    "WATCHDOG_USEC",
    "MEMORY_PRESSURE_WATCH",
    "MEMORY_PRESSURE_WRITE",
)
servers = []


def fail(message):
    raise RuntimeError(message)


def client(socket_path, *arguments, check=True):
    result = subprocess.run(
        [TMUX, "-S", socket_path, *arguments],
        env=BASE_ENV,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=5,
    )
    if check and result.returncode != 0:
        fail(f"tmux {' '.join(arguments)} failed: {result.stderr.strip()}")
    return result


def wait_file(path):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if os.path.isfile(path):
            return
        time.sleep(0.02)
    fail(f"timed out waiting for {path}")


def assert_environment(path, expected):
    with open(path, encoding="utf-8") as stream:
        values = dict(line.rstrip("\n").partition("=")[::2]
                      for line in stream if "=" in line)
    wrong = [name for name, value in expected.items()
             if values.get(name) != value]
    if wrong:
        detail = ", ".join(
            f"{name}={values.get(name)!r}, expected {expected[name]!r}"
            for name in wrong
        )
        fail(f"systemd environment mismatch: {detail}")


def readiness_test():
    notify_path = os.path.join(ROOT, "notify")
    socket_path = os.path.join(ROOT, "ready.sock")
    notify = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    notify.bind(notify_path)
    notify.settimeout(5)
    environment = BASE_ENV.copy()
    environment["NOTIFY_SOCKET"] = notify_path
    process = subprocess.Popen(
        [TMUX, "-D", "-S", socket_path, "-f", "/dev/null"],
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    servers.append((process, socket_path))
    message = notify.recv(4096)
    notify.close()
    if message != b"READY=1":
        fail(f"unexpected readiness datagram: {message!r}")
    client(socket_path, "kill-server")
    if process.wait(timeout=5) != 0:
        fail("readiness server failed")
    servers.remove((process, socket_path))


def environment_test():
    socket_path = os.path.join(ROOT, "environment.sock")
    initial = os.path.join(ROOT, "initial")
    environment = BASE_ENV.copy()
    for name in SYSTEMD_VARIABLES:
        environment[name] = "manager"
    process = subprocess.Popen(
        [TMUX, "-D", "-S", socket_path, "-f", "/dev/null"],
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    servers.append((process, socket_path))

    clean = os.environ["SYSTEMD_ENVIRONMENT_CLEAN"] == "1"
    systemd = os.environ["SYSTEMD_ENABLED"] == "1"
    legacy_removed = {"LISTEN_PID", "LISTEN_FDS", "LISTEN_FDNAMES"}
    initial_removed = set(SYSTEMD_VARIABLES) if clean else set()
    if systemd and not clean:
        initial_removed = legacy_removed
    initial_expected = {
        name: None if name in initial_removed else "manager"
        for name in SYSTEMD_VARIABLES
    }

    client(socket_path, "new-session", "-d", "-s", "handoff",
           f"/bin/sh -c '/usr/bin/env >{initial}; sleep 30'")
    wait_file(initial)
    assert_environment(initial, initial_expected)

    for name in SYSTEMD_VARIABLES:
        client(socket_path, "set-environment", "-g", name, "global")

    global_removed = legacy_removed if systemd and not clean else set()
    global_expected = {
        name: None if name in global_removed else "global"
        for name in SYSTEMD_VARIABLES
    }

    ordinary = os.path.join(ROOT, "ordinary")
    client(socket_path, "new-window", "-d", "-t", "handoff",
           f"/bin/sh -c '/usr/bin/env >{ordinary}; sleep 30'")
    wait_file(ordinary)
    assert_environment(ordinary, global_expected)

    overlay = os.path.join(ROOT, "overlay")
    arguments = ["new-window", "-d", "-P", "-F", "#{pane_id}",
                 "-t", "handoff"]
    for name in SYSTEMD_VARIABLES:
        arguments.extend(("-e", f"{name}=overlay"))
    arguments.append(f"/bin/sh -c '/usr/bin/env >{overlay}; sleep 30'")
    pane = client(socket_path, *arguments).stdout.strip()
    wait_file(overlay)
    overlay_expected = {
        name: "overlay"
        for name in SYSTEMD_VARIABLES
    }
    assert_environment(overlay, overlay_expected)

    respawn = os.path.join(ROOT, "respawn")
    arguments = ["respawn-pane", "-k", "-t", pane]
    for name in SYSTEMD_VARIABLES:
        arguments.extend(("-e", f"{name}=respawn"))
    arguments.append(f"/bin/sh -c '/usr/bin/env >{respawn}; sleep 30'")
    client(socket_path, *arguments)
    wait_file(respawn)
    respawn_expected = {
        name: "respawn"
        for name in SYSTEMD_VARIABLES
    }
    assert_environment(respawn, respawn_expected)

    job = os.path.join(ROOT, "job")
    client(socket_path, "run-shell", "-b", f"/usr/bin/env >{job}")
    wait_file(job)
    assert_environment(job, global_expected)

    popup = os.path.join(ROOT, "popup")
    master, slave = pty.openpty()
    popup_arguments = [
        TMUX, "-S", socket_path, "attach-session", "-t", "handoff", ";",
        "display-popup", "-E",
    ]
    for name in SYSTEMD_VARIABLES:
        popup_arguments.extend(("-e", f"{name}=popup"))
    popup_arguments.append(f"/usr/bin/env >{popup}")
    attached = subprocess.Popen(
        popup_arguments,
        env=BASE_ENV,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)
    try:
        wait_file(popup)
        popup_expected = {
            name: "popup"
            for name in SYSTEMD_VARIABLES
        }
        assert_environment(popup, popup_expected)
    finally:
        if attached.poll() is None:
            attached.terminate()
        try:
            attached.wait(timeout=2)
        except subprocess.TimeoutExpired:
            attached.kill()
            attached.wait()
        os.close(master)

    client(socket_path, "kill-server")
    if process.wait(timeout=5) != 0:
        fail("environment server failed")
    servers.remove((process, socket_path))


try:
    symbols = subprocess.run(
        ["nm", "-g", TMUX], check=True, stdout=subprocess.PIPE, text=True
    ).stdout
    systemd_enabled = os.environ["SYSTEMD_ENABLED"] == "1"
    if systemd_enabled and " systemd_ready\n" not in symbols:
        fail("systemd_ready is missing")
    if not systemd_enabled and " systemd_ready\n" in symbols:
        fail("systemd_ready is present in a disabled build")
    private_restart_symbols = (
        "systemd_restart_prepare",
        "systemd_restart_preflight",
        "systemd_restart_store",
        "systemd_restart_remove",
        "systemd_activation_remove",
        "systemd_restart_ready",
        "systemd_restart_service",
    )
    private_present = {
        name for name in private_restart_symbols if f" {name}\n" in symbols
    }
    if private_present:
        fail("private restart operations are exported")
    accessor_present = " systemd_restart_get_ops\n" in symbols
    restart_enabled = os.environ["SYSTEMD_RESTART_ENABLED"] == "1"
    if restart_enabled != accessor_present:
        fail("restart backend accessor does not match the feature guard")
    if systemd_enabled:
        readiness_test()
    environment_test()
finally:
    for process, socket_path in servers:
        client(socket_path, "kill-server", check=False)
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    shutil.rmtree(ROOT)
PY
