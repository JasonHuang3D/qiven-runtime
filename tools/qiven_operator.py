from __future__ import annotations

import argparse
import concurrent.futures
import ctypes
from dataclasses import dataclass, asdict
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any


# ADR-0046 shim mode: an external repo may delegate to this operator by
# setting QIVEN_TARGET_ROOT to its own repository root (workspace shim +
# pin consumption); unset means this checkout is the target itself.
ROOT = Path(os.environ.get("QIVEN_TARGET_ROOT", Path(__file__).resolve().parents[1])).resolve()
CONFIG_PATH = ROOT / ".qiven" / "operator.json"
HEARTBEAT_SECONDS = 5.0
POLL_SECONDS = 0.05
ANSI_GREEN = "\x1b[32m"
ANSI_RED = "\x1b[31m"
ANSI_YELLOW = "\x1b[33m"
ANSI_CYAN = "\x1b[36m"
ANSI_RESET = "\x1b[0m"


def _flag(name: str) -> bool:
    return os.environ.get(name, "").strip().casefold() in {"1", "true", "yes", "on"}


def _color_enabled() -> bool:
    if _flag("QIVEN_OPERATOR_NO_COLOR") or os.environ.get("NO_COLOR") is not None or not sys.stdout.isatty():
        return False
    if os.name != "nt":
        return True
    try:
        import ctypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)
        mode = ctypes.c_uint()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))
    except Exception:
        return False


@dataclass
class Result:
    name: str
    status: str
    returncode: int
    duration_seconds: float = 0.0
    detail: str = ""
    output: str = ""


class Console:
    def __init__(self, *, json_mode: bool = False, verbose: bool = False, no_color: bool = False) -> None:
        self.json_mode = json_mode
        self.verbose = verbose
        self.color = _color_enabled() and not json_mode and not no_color
        self._lock = threading.Lock()

    def _paint(self, text: str, color: str) -> str:
        return f"{color}{text}{ANSI_RESET}" if self.color else text

    def tag(self, kind: str) -> str:
        mapping = {
            "run": ("[ RUN]", ANSI_CYAN),
            "wait": ("[WAIT]", ANSI_YELLOW),
            "ok": ("[ OK ]", ANSI_GREEN),
            "fail": ("[FAIL]", ANSI_RED),
        }
        text, color = mapping[kind]
        return self._paint(text, color)

    def emit(self, kind: str, message: str) -> None:
        if self.json_mode:
            return
        with self._lock:
            print(f"{self.tag(kind)} {message}", flush=True)

    def block(self, text: str) -> None:
        if self.json_mode or not text:
            return
        with self._lock:
            print(text, end="" if text.endswith("\n") else "\n", flush=True)


class OperatorError(RuntimeError):
    pass


def _load_config() -> dict[str, Any]:
    try:
        data = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise OperatorError(f"missing Operator config: {CONFIG_PATH}") from exc
    except json.JSONDecodeError as exc:
        raise OperatorError(f"invalid Operator config JSON: {exc}") from exc
    if data.get("schema_version") != 1:
        raise OperatorError("unsupported Operator config schema")
    return data


def _run_capture(argv: list[str], *, cwd: Path = ROOT) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)


def _git(*args: str) -> subprocess.CompletedProcess[str]:
    return _run_capture(["git", *args])


# ===========================================================================
# Windows process/job custody layer (exec v2, 2026-09-23 incident redesign)
#
# Production invariants enforced here (docs/design/exec-custody.md):
#   I1 bounded lifetime  - every tree dies by its deadline, kernel-enforced
#   I2 custody on death  - the custodian's death kills its tree instantly
#   I4 tree completeness - the whole tree is one Job Object, no breakaway
#
# The layer is standard-library-only (ctypes). Every helper fails closed:
# a custody primitive that cannot be created means the child is NOT
# spawned (never uncustodied children).
# ===========================================================================

WIN_CREATE_SUSPENDED = 0x00000004
WIN_CREATE_NEW_PROCESS_GROUP = 0x00000200
WIN_CREATE_BREAKAWAY_FROM_JOB = 0x01000000
WIN_CREATE_NO_WINDOW = 0x08000000
WIN_WAIT_OBJECT_0 = 0x00000000
WIN_WAIT_TIMEOUT = 0x00000102
WIN_JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 0x00000008
WIN_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
WIN_JOB_OBJECT_EXTENDED_LIMIT = 9  # JobObjectExtendedLimitInformation class
WIN_JOB_OBJECT_TERMINATE = 0x0008  # access right for OpenJobObject
WIN_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
WIN_SYNCHRONIZE = 0x00100000
# defense-in-depth cap for runaway recursive fan-out inside one run/task
CUSTODY_ACTIVE_PROCESS_CAP = 512
# grace between primary exit and job termination: bounded window for late
# output flush from straggler writers (stderr shares the stdout handle,
# so no offset interleaving hazard exists)
CUSTODY_REAP_GRACE_SECONDS = 1.5


def _load_kernel32():
    import ctypes
    from ctypes import wintypes

    class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("PerProcessUserTimeLimit", wintypes.LARGE_INTEGER),
            ("PerJobUserTimeLimit", wintypes.LARGE_INTEGER),
            ("LimitFlags", wintypes.DWORD),
            ("MinimumWorkingSetSize", ctypes.c_size_t),
            ("MaximumWorkingSetSize", ctypes.c_size_t),
            ("ActiveProcessLimit", wintypes.DWORD),
            ("Affinity", ctypes.c_size_t),
            ("PriorityClass", wintypes.BYTE),
            ("SchedulingClass", wintypes.BYTE),
        ]

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [
            ("ReadOperationCount", ctypes.c_ulonglong),
            ("WriteOperationCount", ctypes.c_ulonglong),
            ("OtherOperationCount", ctypes.c_ulonglong),
            ("ReadTransferCount", ctypes.c_ulonglong),
            ("WriteTransferCount", ctypes.c_ulonglong),
            ("OtherTransferCount", ctypes.c_ulonglong),
        ]

    class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
            ("IoInfo", IO_COUNTERS),
            ("ProcessMemoryLimit", ctypes.c_size_t),
            ("JobMemoryLimit", ctypes.c_size_t),
            ("PeakProcessMemoryUsed", ctypes.c_size_t),
            ("PeakJobMemoryUsed", ctypes.c_size_t),
        ]

    class STARTUPINFOW(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("lpReserved", wintypes.LPWSTR),
            ("lpDesktop", wintypes.LPWSTR),
            ("lpTitle", wintypes.LPWSTR),
            ("dwX", wintypes.DWORD),
            ("dwY", wintypes.DWORD),
            ("dwXSize", wintypes.DWORD),
            ("dwYSize", wintypes.DWORD),
            ("dwXCountChars", wintypes.DWORD),
            ("dwYCountChars", wintypes.DWORD),
            ("dwFillAttribute", wintypes.DWORD),
            ("dwFlags", wintypes.DWORD),
            ("wShowWindow", wintypes.WORD),
            ("cbReserved2", wintypes.WORD),
            ("lpReserved2", ctypes.c_void_p),
            ("hStdInput", wintypes.HANDLE),
            ("hStdOutput", wintypes.HANDLE),
            ("hStdError", wintypes.HANDLE),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.restype = wintypes.HANDLE
    kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel32.SetInformationJobObject.restype = wintypes.BOOL
    kernel32.SetInformationJobObject.argtypes = [
        wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD
    ]
    kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
    kernel32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel32.TerminateJobObject.restype = wintypes.BOOL
    kernel32.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel32.OpenJobObjectW.restype = wintypes.HANDLE
    kernel32.OpenJobObjectW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.ResumeThread.restype = wintypes.DWORD
    kernel32.ResumeThread.argtypes = [wintypes.HANDLE]
    kernel32.WaitForSingleObject.restype = wintypes.DWORD
    kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel32.GetExitCodeProcess.restype = wintypes.BOOL
    kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.CloseHandle.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.GetCurrentProcess.argtypes = []
    return kernel32, JOBOBJECT_EXTENDED_LIMIT_INFORMATION


_KERNEL32_CACHE: Any = None


def _win_kernel32():
    global _KERNEL32_CACHE
    if _KERNEL32_CACHE is None:
        _KERNEL32_CACHE = _load_kernel32()
    return _KERNEL32_CACHE


def _win_last_error() -> str:
    import ctypes

    code = ctypes.get_last_error()
    return f"WinError {code}"


def _job_create(name: str | None = None) -> int:
    """Create a custody Job Object: KILL_ON_JOB_CLOSE + process cap.
    Raises OperatorError on failure (fail-closed: no job, no child)."""
    kernel32, info_class = _win_kernel32()
    handle = kernel32.CreateJobObjectW(None, name)
    if not handle:
        raise OperatorError(f"CreateJobObjectW failed: {_win_last_error()}")
    info = info_class()
    info.BasicLimitInformation.LimitFlags = (
        WIN_JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | WIN_JOB_OBJECT_LIMIT_ACTIVE_PROCESS
    )
    info.BasicLimitInformation.ActiveProcessLimit = CUSTODY_ACTIVE_PROCESS_CAP
    if not kernel32.SetInformationJobObject(
        handle, WIN_JOB_OBJECT_EXTENDED_LIMIT, ctypes.byref(info), ctypes.sizeof(info)
    ):
        kernel32.CloseHandle(handle)
        raise OperatorError(f"SetInformationJobObject failed: {_win_last_error()}")
    return handle


def _job_terminate(handle: int, exit_code: int) -> bool:
    kernel32, _ = _win_kernel32()
    return bool(kernel32.TerminateJobObject(handle, exit_code))


def _job_open_terminate_by_name(name: str, exit_code: int) -> tuple[bool, str]:
    """Terminate a named job from another process. Returns (terminated, note).
    An unopenable job means no handle exists anywhere, which under
    KILL_ON_JOB_CLOSE means the tree is already dead — reported as success
    with that reason, never as a hang."""
    kernel32, _ = _win_kernel32()
    handle = kernel32.OpenJobObjectW(WIN_JOB_OBJECT_TERMINATE, False, name)
    if not handle:
        return True, f"job object {name} not openable (tree already dead): {_win_last_error()}"
    try:
        if not kernel32.TerminateJobObject(handle, exit_code):
            return False, f"TerminateJobObject failed: {_win_last_error()}"
        return True, "job terminated"
    finally:
        kernel32.CloseHandle(handle)


def _win_spawn(argv: list[str], cwd: Path, env: dict[str, str], stdout_fd: int,
               stdin_fd: int, creationflags: int) -> tuple[int, int, int]:
    """Spawn a custodied child through _winapi.CreateProcess — the same
    battle-tested path subprocess itself uses. Returns (process_handle,
    thread_handle, pid); caller resumes+closes the thread and closes the
    process handle when done. The std fds MUST already be inheritable
    (PEP 446: os.open fds are non-inheritable by default); this function
    asserts that contract rather than silently spawning blind children.
    Raises OSError on failure."""
    import _winapi
    import msvcrt

    os.set_inheritable(stdin_fd, True)
    os.set_inheritable(stdout_fd, True)
    # bInheritHandles=TRUE hands the child EVERY inheritable handle of
    # this process, not just the std trio — including OUR OWN stdout/stdin
    # when the caller supervises us through a pipe. Descendants then keep
    # that pipe open and the caller blocks until the whole run tree exits
    # (observed live: a 1-second supervision budget took 13.8 s to return).
    # De-inherit our stdio first so only the intended handles propagate.
    for std_fd in (0, 1, 2):
        try:
            if os.get_inheritable(std_fd):
                os.set_inheritable(std_fd, False)
        except (OSError, ValueError):
            pass
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= _winapi.STARTF_USESTDHANDLES
    # hStd* fields carry HANDLES; an fd number there is silently invalid
    # (the child loses its redirected output — the empty-log defect class)
    startup.hStdInput = msvcrt.get_osfhandle(stdin_fd)
    startup.hStdOutput = msvcrt.get_osfhandle(stdout_fd)
    startup.hStdError = msvcrt.get_osfhandle(stdout_fd)
    handle, thread, pid, _tid = _winapi.CreateProcess(
        None,
        subprocess.list2cmdline(argv),
        None,
        None,
        True,
        creationflags,
        env,
        str(cwd) if cwd else None,
        startup,
    )
    return handle, thread, pid


def _wait_handle(handle: int, milliseconds: int) -> str:
    """'signaled' | 'timeout' | 'failed' — wait-object liveness without the
    STILL_ACTIVE(259) exit-code ambiguity."""
    kernel32, _ = _win_kernel32()
    result = kernel32.WaitForSingleObject(handle, milliseconds)
    if result == WIN_WAIT_OBJECT_0:
        return "signaled"
    if result == WIN_WAIT_TIMEOUT:
        return "timeout"
    return "failed"


def _exit_code_of_handle(handle: int) -> int:
    import ctypes
    from ctypes import wintypes

    kernel32, _ = _win_kernel32()
    code = wintypes.DWORD()
    if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
        raise OperatorError(f"GetExitCodeProcess failed: {_win_last_error()}")
    return int(code.value)


def _process_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        kernel32, _ = _win_kernel32()
        handle = kernel32.OpenProcess(WIN_PROCESS_QUERY_LIMITED_INFORMATION | WIN_SYNCHRONIZE, False, pid)
        if not handle:
            return False
        try:
            # wait-based check: a process that exited with code 259 is DEAD
            # (the old GetExitCodeProcess==STILL_ACTIVE check misread it)
            return _wait_handle(handle, 0) == "timeout"
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _utc_now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _utc_timestamp_seconds(stamp: object) -> float | None:
    """Parse an RFC3339-UTC second stamp; None when absent or malformed."""
    if not isinstance(stamp, str) or not stamp:
        return None
    import calendar

    try:
        parsed = time.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError:
        return None
    return float(calendar.timegm(parsed))


def _utc_now_seconds() -> float:
    import calendar

    return float(calendar.timegm(time.gmtime()))


def _heartbeat_age_seconds(record: dict[str, Any]) -> float | None:
    seconds = _utc_timestamp_seconds(record.get("heartbeat_utc") or record.get("started_utc"))
    if seconds is None:
        return None
    return max(0.0, _utc_now_seconds() - seconds)


# ===========================================================================
# exec v2: supervised detached execution under bounded process custody.
#
# Front-end (qiven exec start): builds the run record, spawns the WATCHDOG
# detached, monitors with heartbeats until its own --timeout budget
# (exit 124 = still running), and returns. The watchdog is the custodian:
# it creates the run's Job Object (KILL_ON_JOB_CLOSE), joins the job
# itself (so its death kills the tree by kernel action), spawns the child
# born into the job, enforces the deadline lease, reaps tree leftovers
# after the primary exits (the MSBuild node-reuse leak class), and
# terminates the job — itself included — with the child's exit code.
#
# Window discipline (2026-09-23 law, unchanged): children and watchdog use
# CREATE_NO_WINDOW, never DETACHED_PROCESS; .cmd/.bat targets run through
# an explicit `cmd.exe /d /c call <abs path>`; path-like argv[0] resolves
# against ROOT before spawning (MEM-20260923T212000Z-D4E5F6).
# ===========================================================================
EXEC_DEFAULT_TIMEOUT_SECONDS = 120.0
EXEC_DEFAULT_MAX_LIFETIME_SECONDS = 3600.0
EXEC_MAX_LIFETIME_CEILING_SECONDS = 86400.0
EXEC_MAX_LIFETIME_FLOOR_SECONDS = 10.0
EXEC_EXIT_STILL_RUNNING = 124
EXEC_WATCHDOG_STARTUP_BUDGET_SECONDS = 15.0
EXEC_HEARTBEAT_STALE_SECONDS = 45.0
_BATCH_SUFFIXES = (".cmd", ".bat")
_REDACTED_ENV_KEYS = ()


def _exec_creationflags(breakaway: bool = True) -> int:
    """Windows creation flags for the detached WATCHDOG spawn. breakaway is
    best-effort survival across a caller's job death; custody never depends
    on it (the lease does)."""
    if os.name != "nt":
        return 0
    flags = WIN_CREATE_NO_WINDOW | WIN_CREATE_NEW_PROCESS_GROUP
    if breakaway:
        flags |= WIN_CREATE_BREAKAWAY_FROM_JOB
    return flags


def _child_creationflags() -> int:
    """Creation flags for a CUSTODIED child: hidden console + own group; NO
    breakaway (the child must stay inside the run's job — tree
    completeness, invariant I4)."""
    if os.name != "nt":
        return 0
    return WIN_CREATE_NO_WINDOW | WIN_CREATE_NEW_PROCESS_GROUP


def _exec_prepare_argv(argv: list[str]) -> list[str]:
    """Harden the exec target before spawning.

    1. A path-like argv[0] (absolute, or containing a separator) that
       exists relative to the operator ROOT is resolved to its absolute
       path — the child's CreateProcess resolves the APPLICATION against
       the CALLER's directories, not the child's cwd, so a relative path
       that looks valid can still WinError 2.
    2. .cmd/.bat targets run through an explicit `cmd.exe /d /c call ...`
       (/d skips AutoRun registry scripts; the explicit form replaces
       CreateProcess's implicit — and undocumented — batch dispatch, and
       makes the console-handling path deterministic).
    """
    if not argv:
        return argv
    head = argv[0]
    rest = argv[1:]
    resolved = head
    if os.path.isabs(head) or ("/" in head) or ("\\" in head):
        candidate = Path(head)
        if not candidate.is_absolute():
            candidate = ROOT / head
        if candidate.is_file():
            resolved = candidate.resolve()
    if str(resolved).lower().endswith(_BATCH_SUFFIXES):
        return ["cmd.exe", "/d", "/c", "call", str(resolved), *rest]
    return [str(resolved), *rest]


def _exec_dir() -> Path:
    return ROOT / ".generated-temp" / "operator" / "exec"


def _new_exec_id() -> str:
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    return f"{stamp}-{os.getpid():08d}-{os.urandom(3).hex()}"


def _exec_record_path(exec_id: str) -> Path:
    return _exec_dir() / f"{exec_id}.json"


def _exec_log_path(exec_id: str) -> Path:
    return _exec_dir() / f"{exec_id}.log"


RECORD_WRITE_ATTEMPTS = 10
RECORD_WRITE_RETRY_SECONDS = 0.02


def _write_exec_record(record: dict[str, Any]) -> None:
    """Atomic record rewrite (tmp + os.replace) with reader-collision
    retry. On Windows the replace fails with Access Denied while another
    process (the supervising front-end polls every 50 ms) briefly holds
    the file open — Python readers cannot request FILE_SHARE_DELETE, so
    the WRITER must retry. A write that still fails after the retries
    degrades to a truncating direct write (torn-read risk beats losing
    the custodian: record writes must never crash a live watchdog)."""
    path = _exec_record_path(str(record.get("id")))
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f"{path.name}.tmp-{os.getpid()}")
    tmp.write_text(json.dumps(record, ensure_ascii=False, sort_keys=True, indent=1), encoding="utf-8")
    for attempt in range(RECORD_WRITE_ATTEMPTS):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt + 1 == RECORD_WRITE_ATTEMPTS:
                path.write_text(
                    json.dumps(record, ensure_ascii=False, sort_keys=True, indent=1),
                    encoding="utf-8",
                )
                try:
                    tmp.unlink()
                except OSError:
                    pass
                return
            time.sleep(RECORD_WRITE_RETRY_SECONDS)


def _read_exec_record(exec_id: str) -> dict[str, Any]:
    path = _exec_record_path(exec_id)
    if not path.is_file():
        raise OperatorError(f"unknown exec id: {exec_id} (no record at {path})")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise OperatorError(f"corrupt exec record: {path}") from exc


def _read_exec_record_path(path: Path) -> dict[str, Any] | None:
    """Best-effort record read for observers (sweeps, lists, monitors).
    A momentary PermissionError is the Windows rename-collision surface,
    not corruption — retry briefly before concluding the record unreadable."""
    last_error: OSError | None = None
    for attempt in range(5):
        try:
            record = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return None
        except PermissionError as exc:
            last_error = exc
            time.sleep(0.01)
            continue
        except (json.JSONDecodeError, OSError):
            return None
        return record if isinstance(record, dict) else None
    del last_error
    return None


_TERMINAL_STATES = frozenset({"done", "stopped", "expired", "error", "indeterminate"})


def _exec_state(record: dict[str, Any]) -> str:
    """Terminal states come from the record; live states are computed from
    custody evidence (watchdog/pid liveness + heartbeat freshness). Never
    guesses an exit code for an unobserved exit. Schema-v1 records (no
    status field) are honored: an observed exit_code means done."""
    if record.get("exit_code") is not None:
        return "done"
    status = str(record.get("status") or "")
    if status in _TERMINAL_STATES:
        return status
    if status == "starting":
        return "starting"
    pid = int(record.get("pid") or 0)
    watchdog_pid = int(record.get("watchdog_pid") or 0)
    watchdog_alive = _process_alive(watchdog_pid)
    primary_alive = _process_alive(pid)
    if watchdog_alive:
        return "running" if primary_alive else "reaping"
    if primary_alive:
        # custody anomaly: kill-on-close should have reaped the tree when
        # the watchdog died; sweep will taskkill it as defense in depth
        return "orphaned"
    return "indeterminate"


def _exec_snapshot(record: dict[str, Any]) -> dict[str, Any]:
    pid = int(record.get("pid") or 0)
    log_path = Path(str(record.get("log") or ""))
    log_size = log_path.stat().st_size if log_path.is_file() else 0
    state = _exec_state(record)
    heartbeat_age = _heartbeat_age_seconds(record)
    return {
        "id": record.get("id"),
        "pid": pid,
        "watchdog_pid": int(record.get("watchdog_pid") or 0),
        "job_name": record.get("job_name"),
        "state": state,
        "exit_code": record.get("exit_code"),
        "argv": record.get("argv"),
        "log": str(log_path),
        "log_bytes": log_size,
        "started_utc": record.get("started_utc"),
        "finished_utc": record.get("finished_utc"),
        "deadline_utc": record.get("deadline_utc"),
        "heartbeat_age_seconds": None if heartbeat_age is None else round(heartbeat_age, 1),
    }


def _tail_text(path: Path, limit_bytes: int) -> str:
    if not path.is_file():
        return ""
    size = path.stat().st_size
    with path.open("rb") as handle:
        if size > limit_bytes:
            handle.seek(-limit_bytes, os.SEEK_END)
        data = handle.read(limit_bytes)
    return data.decode("utf-8", errors="replace")


def _child_environment() -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("PYTHONUTF8", "1")
    env.setdefault("PYTHONIOENCODING", "utf-8")
    # node-reuse is the observed leak amplifier (v19 incident): even where
    # custody is somehow unavailable, our children never leave MSBuild
    # worker nodes behind
    env["MSBUILDDISABLENODEREUSE"] = "1"
    return env


def _clamp_lifetime(value: float) -> float:
    return min(EXEC_MAX_LIFETIME_CEILING_SECONDS, max(EXEC_MAX_LIFETIME_FLOOR_SECONDS, value))


# --- watchdog (the custodian) ---------------------------------------------


def _watchdog_run(record_path: str) -> int:
    """`python qiven_operator.py --exec-watchdog <record>`: supervise one
    exec run under kernel custody. Owns the run record while running.
    Exit code mirrors the child's; 2 for operator errors before spawn."""
    record = _read_exec_record_path(Path(record_path))
    if record is None:
        sys.stderr.write(f"[watchdog] unreadable record: {record_path}\n")
        return 2
    log_path = Path(str(record.get("log")))
    job_name = str(record.get("job_name"))
    try:
        max_lifetime = float(record.get("max_lifetime_seconds") or 0.0)
    except (TypeError, ValueError):
        max_lifetime = 0.0
    if max_lifetime <= 0.0:
        record["status"] = "error"
        record["error"] = "watchdog record lacks max_lifetime_seconds"
        _write_exec_record(record)
        return 2

    if os.name == "nt":
        try:
            job = _job_create(job_name)
            kernel32, _ = _win_kernel32()
            # join our own job: every descendant is born inside it, and our
            # death (any cause) closes the last handle -> kernel tree-kill
            if not kernel32.AssignProcessToJobObject(job, kernel32.GetCurrentProcess()):
                raise OperatorError(
                    f"AssignProcessToJobObject(self) failed: {_win_last_error()}"
                )
        except OperatorError as exc:
            record["status"] = "error"
            record["error"] = str(exc)
            _write_exec_record(record)
            sys.stderr.write(f"[watchdog] {exc}\n")
            return 2

    spawn_argv = _exec_prepare_argv(list(record.get("argv") or []))
    if not spawn_argv:
        record["status"] = "error"
        record["error"] = "exec requires a command after --"
        _write_exec_record(record)
        return 2

    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    process_handle = None
    try:
        if os.name == "nt":
            log_fd = os.open(str(log_path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
            nul_fd = os.open("NUL", os.O_RDWR)
            try:
                process_handle, thread_handle, pid = _win_spawn(
                    spawn_argv, ROOT, _child_environment(), log_fd, nul_fd,
                    _child_creationflags(),
                )
            finally:
                os.close(log_fd)
                os.close(nul_fd)
            kernel32, _ = _win_kernel32()
            kernel32.ResumeThread(thread_handle)
            kernel32.CloseHandle(thread_handle)
        else:
            with log_path.open("wb") as log:
                process = subprocess.Popen(
                    spawn_argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                    stdin=subprocess.DEVNULL, env=_child_environment(),
                    start_new_session=True,
                )
            pid = process.pid
    except OSError as exc:
        record["status"] = "error"
        record["error"] = f"could not start exec process: {exc}"
        record["spawn_argv"] = spawn_argv
        _write_exec_record(record)
        return 2

    record["pid"] = pid
    record["spawn_argv"] = spawn_argv
    record["status"] = "running"
    record["started_utc"] = _utc_now()
    record["watchdog_pid"] = os.getpid()
    record["heartbeat_utc"] = _utc_now()
    _write_exec_record(record)

    next_heartbeat = time.monotonic() + HEARTBEAT_SECONDS
    deadline_monotonic = time.monotonic() + max_lifetime
    while True:
        if os.name == "nt":
            state = _wait_handle(process_handle, 100)
            finished = state == "signaled"
            if state == "failed":
                record["status"] = "error"
                record["error"] = "WaitForSingleObject failed on child handle"
                _write_exec_record(record)
                return 2
        else:
            finished = process.poll() is not None
            time.sleep(0.1)
        now = time.monotonic()
        if finished:
            break
        if now >= deadline_monotonic:
            # lease enforcement: terminal record FIRST (durable before we
            # terminate ourselves inside the job), then kernel kill
            record["status"] = "expired"
            record["finished_utc"] = _utc_now()
            record["heartbeat_utc"] = _utc_now()
            _write_exec_record(record)
            if os.name == "nt":
                _job_terminate(job, EXEC_EXIT_STILL_RUNNING)
                return EXEC_EXIT_STILL_RUNNING  # if the job-kill self did not land
            import signal

            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass
            return EXEC_EXIT_STILL_RUNNING
        if now >= next_heartbeat:
            record["heartbeat_utc"] = _utc_now()
            _write_exec_record(record)
            next_heartbeat = now + HEARTBEAT_SECONDS

    if os.name == "nt":
        exit_code = _exit_code_of_handle(process_handle)
    else:
        exit_code = int(process.returncode)
    duration = time.monotonic() - started
    # terminal record BEFORE terminating the job (the termination also
    # ends this watchdog — the record must already be durable)
    record["status"] = "done"
    record["exit_code"] = exit_code
    record["finished_utc"] = _utc_now()
    record["duration_seconds"] = round(duration, 3)
    record["heartbeat_utc"] = _utc_now()
    _write_exec_record(record)
    # completion reaps: bounded grace for straggler writers (MSBuild node
    # reuse class), then the whole tree — leftovers and this watchdog —
    # exits with the child's code
    time.sleep(CUSTODY_REAP_GRACE_SECONDS)
    if os.name == "nt":
        _job_terminate(job, exit_code)
        kernel32, _ = _win_kernel32()
        kernel32.CloseHandle(process_handle)
        return exit_code
    import signal

    try:
        os.killpg(pid, signal.SIGKILL)
    except OSError:
        pass
    return exit_code


# --- exec front-end --------------------------------------------------------


def _spawn_watchdog(record_path: Path, diag_path: Path) -> int:
    """Spawn the watchdog detached; returns its pid. Diagnostics land in a
    durable side log for postmortems (empty logs are a defect class)."""
    argv = [sys.executable, str(Path(__file__).resolve()), "--exec-watchdog", str(record_path)]
    if os.name == "nt":
        diag_fd = os.open(str(diag_path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
        nul_fd = os.open("NUL", os.O_RDWR)
        try:
            flags = _exec_creationflags(breakaway=True)
            try:
                handle, thread, pid = _win_spawn(argv, ROOT, _child_environment(), diag_fd, nul_fd, flags)
            except OSError:
                # restrictive ancestor job forbids breakaway: retry without
                # it — custody never depended on breakaway anyway
                handle, thread, pid = _win_spawn(
                    argv, ROOT, _child_environment(), diag_fd, nul_fd,
                    _exec_creationflags(breakaway=False),
                )
        finally:
            os.close(diag_fd)
            os.close(nul_fd)
        kernel32, _ = _win_kernel32()
        kernel32.ResumeThread(thread)
        kernel32.CloseHandle(thread)
        kernel32.CloseHandle(handle)
        return pid
    with diag_path.open("wb") as diag:
        process = subprocess.Popen(
            argv, cwd=ROOT, stdout=diag, stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL, env=_child_environment(), start_new_session=True,
        )
    return process.pid


def _exec_terminal_response(exec_id: str, current: dict[str, Any], console: Console) -> tuple[dict[str, Any], int]:
    """Map a terminal run state to its front-end payload and exit code."""
    state = _exec_state(current)
    snapshot = _exec_snapshot(current)
    if state == "done":
        code = int(current.get("exit_code") or 0)
        console.emit("ok" if code == 0 else "fail",
                     f"exec {exec_id}: exit {code} ({current.get('duration_seconds', 0)}s)")
        return dict(snapshot, status="done"), code
    if state == "expired":
        console.emit("fail", f"exec {exec_id}: lease expired at {current.get('deadline_utc')} (tree killed, code unknown)")
        return dict(snapshot, status="expired"), 1
    if state == "error":
        detail = str(current.get("error") or "watchdog error")
        console.emit("fail", f"exec {exec_id}: {detail}")
        return dict(snapshot, status="error"), 2
    if state == "stopped":
        console.emit("ok", f"exec {exec_id}: stopped")
        return dict(snapshot, status="stopped"), 0
    console.emit("wait", f"exec {exec_id}: indeterminate exit (no living supervisor observed it)")
    return dict(snapshot, status="indeterminate"), EXEC_EXIT_STILL_RUNNING


def _exec_start_frontend(argv: list[str], timeout_seconds: float, max_lifetime: float,
                         console: Console) -> tuple[dict[str, Any], int]:
    if not argv:
        raise OperatorError("exec requires a command after --")
    max_lifetime = _clamp_lifetime(max_lifetime)
    timeout_seconds = max(1.0, min(timeout_seconds, max_lifetime))
    exec_id = _new_exec_id()
    log_path = _exec_log_path(exec_id)
    record_path = _exec_record_path(exec_id)
    diag_path = _exec_dir() / f"{exec_id}.watchdog.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    deadline_utc = time.strftime(
        "%Y-%m-%dT%H:%M:%SZ", time.gmtime(_utc_now_seconds() + max_lifetime)
    )
    record: dict[str, Any] = {
        "schema": 2,
        "id": exec_id,
        "argv": list(argv),
        "log": str(log_path),
        "job_name": f"qiven-exec-{exec_id}",
        "started_utc": _utc_now(),
        "timeout_seconds": timeout_seconds,
        "max_lifetime_seconds": max_lifetime,
        "deadline_utc": deadline_utc,
        "status": "starting",
    }
    _write_exec_record(record)

    try:
        watchdog_pid = _spawn_watchdog(record_path, diag_path)
    except OSError as exc:
        record["status"] = "error"
        record["error"] = f"could not spawn watchdog: {exc}"
        _write_exec_record(record)
        detail = f"exec start failed (watchdog spawn): {exc}"
        console.emit("fail", detail)
        return {"status": "error", "error": detail, "argv": argv}, 2
    # from here the WATCHDOG owns the record: its first write carries pid,
    # watchdog_pid and state=running. A front-end rewrite here would race
    # that write and could regress the record to "starting".
    console.emit("run", f"exec {exec_id}: watchdog pid {watchdog_pid}, lease {max_lifetime:.0f}s, log {log_path}")

    started = time.monotonic()

    # phase 1: wait for the watchdog to signal (record gains pid + state)
    while True:
        current = _read_exec_record_path(record_path) or record
        state = _exec_state(current)
        if state != "starting":
            record = current
            break
        if time.monotonic() - started >= EXEC_WATCHDOG_STARTUP_BUDGET_SECONDS:
            detail = (f"watchdog {watchdog_pid} did not signal within "
                      f"{EXEC_WATCHDOG_STARTUP_BUDGET_SECONDS:.0f}s "
                      f"(diagnostics: {diag_path})")
            console.emit("fail", f"exec {exec_id}: {detail}")
            current = _read_exec_record_path(record_path) or record
            current["status"] = "error"
            current["error"] = detail
            _write_exec_record(current)
            _kill_tree_hard(watchdog_pid)
            return {"status": "error", "error": detail, "id": exec_id, "argv": argv}, 2
        time.sleep(POLL_SECONDS)

    # phase 2: supervise until a terminal state or the front-end budget.
    # The run itself is bounded by the LEASE, never by this loop: when the
    # budget elapses the watchdog keeps custody and enforces the deadline.
    next_heartbeat = time.monotonic() + HEARTBEAT_SECONDS
    while True:
        current = _read_exec_record_path(record_path) or record
        record = current
        state = _exec_state(current)
        if state in _TERMINAL_STATES:
            return _exec_terminal_response(exec_id, current, console)
        now = time.monotonic()
        if now - started >= timeout_seconds:
            snapshot = _exec_snapshot(current)
            console.emit(
                "wait",
                f"exec {exec_id}: still running after {timeout_seconds:.0f}s "
                f"(log {snapshot['log_bytes']} bytes); operator returns; lease {deadline_utc}",
            )
            return dict(snapshot, status="still-running"), EXEC_EXIT_STILL_RUNNING
        if now >= next_heartbeat:
            log_bytes = log_path.stat().st_size if log_path.is_file() else 0
            age = _heartbeat_age_seconds(current)
            console.emit(
                "wait",
                f"exec {exec_id}: running for {now - started:.0f}s, log {log_bytes} bytes"
                + (f", custodian beat {age:.0f}s ago" if age is not None else ""),
            )
            next_heartbeat = now + HEARTBEAT_SECONDS
        time.sleep(POLL_SECONDS)


def _kill_tree_hard(pid: int) -> None:
    """Last-resort tree kill (sweep defense in depth; not the primary
    custody path)."""
    if pid <= 0:
        return
    if os.name == "nt":
        _run_capture(["taskkill", "/T", "/F", "/PID", str(pid)])
    else:
        import signal

        try:
            os.killpg(pid, signal.SIGKILL)
        except OSError:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass


def _exec_stop(record: dict[str, Any], console: Console) -> tuple[dict[str, Any], int]:
    exec_id = str(record.get("id"))
    state = _exec_state(record)
    if state in _TERMINAL_STATES:
        snapshot = _exec_snapshot(record)
        snapshot["status"] = "not-running" if state == "indeterminate" else state
        console.emit("ok", f"exec stop {exec_id}: already {state}")
        return snapshot, 0
    job_name = str(record.get("job_name") or "")
    # durable terminal record FIRST, then the kernel action
    record["status"] = "stopped"
    record["finished_utc"] = _utc_now()
    _write_exec_record(record)
    note = "no job name on record"
    if os.name == "nt" and job_name:
        terminated, note = _job_open_terminate_by_name(job_name, 130)
    else:
        _kill_tree_hard(int(record.get("pid") or 0))
        terminated, note = True, "tree killed"
    snapshot = _exec_snapshot(record)
    snapshot["stop_note"] = note
    snapshot["status"] = "stopped"
    console.emit("ok" if terminated else "fail", f"exec stop {exec_id}: stopped ({note})")
    return snapshot, 0 if terminated else 1


def _sweep_exec_records(console: Console | None = None, *, quiet: bool = True) -> list[dict[str, Any]]:
    """Dead-man insurance, piggybacked on every operator invocation:
    terminate runs past their lease, hard-kill custody anomalies, finalize
    stale records. Never raises; never touches healthy runs."""
    actions: list[dict[str, Any]] = []
    directory = _exec_dir()
    if not directory.is_dir():
        return actions
    now_seconds = _utc_now_seconds()
    for record_path in sorted(directory.glob("*.json")):
        record = _read_exec_record_path(record_path)
        if record is None:
            continue
        status = str(record.get("status") or "")
        if status in _TERMINAL_STATES:
            continue
        state = _exec_state(record)
        if state == "orphaned":
            _kill_tree_hard(int(record.get("pid") or 0))
            record["status"] = "stopped"
            record["finished_utc"] = _utc_now()
            record["stop_note"] = "sweep: custody anomaly hard-killed"
            _write_exec_record(record)
            actions.append({"id": record.get("id"), "action": "hard-killed"})
        elif state == "indeterminate":
            record["status"] = "indeterminate"
            record["finished_utc"] = _utc_now()
            _write_exec_record(record)
            actions.append({"id": record.get("id"), "action": "finalized-indeterminate"})
        else:
            deadline = _utc_timestamp_seconds(str(record.get("deadline_utc") or ""))
            if deadline is not None and now_seconds > deadline:
                job_name = str(record.get("job_name") or "")
                if os.name == "nt" and job_name:
                    _job_open_terminate_by_name(job_name, EXEC_EXIT_STILL_RUNNING)
                else:
                    _kill_tree_hard(int(record.get("pid") or 0))
                record["status"] = "expired"
                record["finished_utc"] = _utc_now()
                _write_exec_record(record)
                actions.append({"id": record.get("id"), "action": "expired"})
    if actions and console is not None and not quiet:
        for action in actions:
            console.emit("wait", f"exec sweep: {action['action']} {action['id']}")
    return actions


def _toolchain() -> dict[str, str]:
    root = Path(os.environ.get("QIVEN_TOOLCHAIN_ROOT", ROOT.parent / "qiven-toolchain-win")).resolve()
    manifest = root / "toolchain.json"
    if not manifest.is_file():
        raise OperatorError(f"toolchain manifest not found: {manifest}")
    try:
        data = json.loads(manifest.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise OperatorError(f"invalid toolchain manifest: {exc}") from exc
    tools = data.get("tools", {})
    resolved: dict[str, str] = {"toolchain_root": str(root)}
    for logical, key in (("cmake", "cmake"), ("clang_format", "clang-format")):
        entry = tools.get(key)
        if not isinstance(entry, dict) or not entry.get("path"):
            raise OperatorError(f"toolchain manifest lacks {key}")
        path = (root / entry["path"]).resolve()
        if not path.is_file():
            raise OperatorError(f"tool not found: {path}")
        resolved[logical] = str(path)
    ctest = Path(resolved["cmake"]).with_name("ctest.exe" if os.name == "nt" else "ctest")
    if not ctest.is_file():
        raise OperatorError(f"ctest not found beside CMake: {ctest}")
    resolved["ctest"] = str(ctest)
    return resolved


def _expand(value: str, tools: dict[str, str] | None) -> str:
    if "{" not in value:
        return value
    mapping = {"root": str(ROOT), "python": sys.executable}
    if tools:
        mapping.update(tools)
    try:
        return value.format_map(mapping)
    except KeyError as exc:
        raise OperatorError(f"unknown command placeholder: {exc.args[0]}") from exc


def _argv_for_task(spec: dict[str, Any]) -> list[str]:
    raw = spec.get("argv")
    if not isinstance(raw, list) or not raw or not all(isinstance(item, str) and item for item in raw):
        raise OperatorError("command task requires non-empty string argv")
    needs_tools = any("{cmake}" in item or "{ctest}" in item or "{clang_format}" in item or "{toolchain_root}" in item for item in raw)
    tools = _toolchain() if needs_tools else None
    argv = [_expand(item, tools) for item in raw]
    candidate = Path(argv[0])
    if not candidate.is_absolute():
        local = ROOT / candidate
        if local.exists():
            argv[0] = str(local)
    if argv[0].lower().endswith((".cmd", ".bat")):
        if os.name != "nt":
            raise OperatorError("Windows batch task requested on non-Windows host")
        command_line = subprocess.list2cmdline(argv)
        return ["cmd.exe", "/d", "/s", "/c", f"call {command_line}"]
    return argv


def _run_process(name: str, spec: dict[str, Any], console: Console) -> Result:
    """Run ONE declared task child under per-task process custody: a Job
    Object (KILL_ON_JOB_CLOSE + process cap) held by this operator. The
    tree cannot outlive the task: on primary exit, leftovers (the MSBuild
    node-reuse class) are terminated after the output grace; if THIS
    operator dies mid-task, the kernel kills the tree via handle close."""
    started = time.monotonic()
    console.emit("run", name)
    try:
        argv = _argv_for_task(spec)
    except OperatorError as exc:
        console.emit("fail", f"{name}: {exc}")
        return Result(name, "fail", 2, detail=str(exc))

    env = _child_environment()
    log_fd = None
    job = None
    process_handle = None
    process = None
    returncode: int | None = None
    with tempfile.NamedTemporaryFile(prefix="qiven-operator-", suffix=".log", delete=False) as handle:
        log_path = Path(handle.name)
    try:
        if os.name == "nt":
            job = _job_create()
            log_fd = os.open(str(log_path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC)
            nul_fd = os.open("NUL", os.O_RDWR)
            try:
                # CREATE_SUSPENDED -> assign -> resume: zero-race custody
                # (no grandchild can be born before the primary is in the
                # job, unlike the Popen-then-assign pattern)
                process_handle, thread_handle, _pid = _win_spawn(
                    argv, ROOT, env, log_fd, nul_fd,
                    _child_creationflags() | WIN_CREATE_SUSPENDED,
                )
            finally:
                os.close(log_fd)
                os.close(nul_fd)
            kernel32, _ = _win_kernel32()
            kernel32.AssignProcessToJobObject(job, process_handle)
            kernel32.ResumeThread(thread_handle)
            kernel32.CloseHandle(thread_handle)
        else:
            with log_path.open("wb") as log:
                process = subprocess.Popen(
                    argv, cwd=ROOT, env=env, stdout=log,
                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                    start_new_session=True,
                )
        next_heartbeat = time.monotonic() + HEARTBEAT_SECONDS
        while True:
            if os.name == "nt":
                if _wait_handle(process_handle, 100) == "signaled":
                    returncode = _exit_code_of_handle(process_handle)
                    break
            else:
                returncode = process.poll()
                if returncode is not None:
                    break
                time.sleep(0.1)
            now = time.monotonic()
            if now >= next_heartbeat:
                console.emit("wait", f"{name}: running for {now - started:.0f}s")
                next_heartbeat = now + HEARTBEAT_SECONDS
        duration = time.monotonic() - started
        # completion reaps the task tree (bounded grace for late flush)
        time.sleep(CUSTODY_REAP_GRACE_SECONDS)
        output = log_path.read_text(encoding="utf-8", errors="replace")
        if os.name == "nt":
            _job_terminate(job, int(returncode))
        else:
            import signal

            try:
                os.killpg(int(process.pid), signal.SIGKILL)
            except OSError:
                pass
        assert returncode is not None
        if returncode == 0:
            console.emit("ok", f"{name} ({duration:.2f}s)")
            if console.verbose:
                console.block(output)
            return Result(name, "pass", 0, duration, output=output)
        console.emit("fail", f"{name}: exit {returncode} ({duration:.2f}s)")
        console.block(output)
        return Result(name, "fail", int(returncode), duration, output=output)
    except OSError as exc:
        detail = f"could not start process: {exc}"
        console.emit("fail", f"{name}: {detail}")
        return Result(name, "fail", 2, time.monotonic() - started, detail=detail)
    finally:
        if os.name == "nt":
            kernel32, _ = _win_kernel32()
            if process_handle:
                kernel32.CloseHandle(process_handle)
            if job:
                kernel32.CloseHandle(job)
        try:
            log_path.unlink()
        except OSError:
            pass


def _builtin(name: str, spec: dict[str, Any], console: Console, expect_head: str | None) -> Result:
    started = time.monotonic()
    console.emit("run", name)
    kind = spec.get("builtin")
    if kind == "exact_head":
        expected = expect_head or spec.get("expected")
        if not expected:
            console.emit("ok", f"{name}: no expected SHA supplied")
            return Result(name, "pass", 0, time.monotonic() - started)
        completed = _git("rev-parse", "HEAD")
        actual = completed.stdout.strip()
        if completed.returncode or actual != expected:
            detail = f"expected {expected}, got {actual or '<unresolved>'}"
            console.emit("fail", f"{name}: {detail}")
            return Result(name, "fail", 1, time.monotonic() - started, detail=detail, output=completed.stdout)
    elif kind == "gate_proof":
        # pit P-53: merge-class publication requires a recorded full-gate
        # PASS for the EXACT head being published; a missing or stale receipt
        # fails closed (the A3 masking class: scoped gates are not the gate)
        gate_name = str(spec.get("gate") or "")
        head = _git("rev-parse", "HEAD").stdout.strip()
        receipt_file = _receipt_path(gate_name, head)
        if not gate_name:
            detail = "gate_proof builtin requires a 'gate' name"
            console.emit("fail", f"{name}: {detail}")
            return Result(name, "fail", 1, time.monotonic() - started, detail=detail)
        if not receipt_file.is_file():
            detail = f"no {gate_name} PASS receipt for exact head {head[:12]}"
            console.emit("fail", f"{name}: {detail}")
            return Result(name, "fail", 1, time.monotonic() - started, detail=detail)
        receipt = json.loads(receipt_file.read_text(encoding="utf-8"))
        if receipt.get("status") != "pass" or receipt.get("head") != head:
            detail = f"receipt for {head[:12]} is not a matching PASS"
            console.emit("fail", f"{name}: {detail}")
            return Result(name, "fail", 1, time.monotonic() - started, detail=detail)
        console.emit("ok", f"{name}: {gate_name} PASS @ {head[:12]} "
                           f"({receipt.get('timestamp')})")
        return Result(name, "pass", 0, time.monotonic() - started)
    elif kind == "git_diff_check":
        base = str(spec.get("base", "origin/main"))
        completed = _git("diff", "--check", f"{base}...HEAD")
        if completed.returncode:
            console.emit("fail", f"{name}: git diff --check failed")
            console.block(completed.stdout)
            return Result(name, "fail", completed.returncode, time.monotonic() - started, output=completed.stdout)
    elif kind == "git_clean_tree":
        completed = _git("status", "--porcelain=v1", "--untracked-files=all")
        dirty = completed.stdout.strip()
        if completed.returncode or dirty:
            detail = "working tree is not clean" if dirty else "git status failed"
            console.emit("fail", f"{name}: {detail}")
            console.block(completed.stdout)
            return Result(name, "fail", 1, time.monotonic() - started, detail=detail, output=completed.stdout)
    else:
        detail = f"unknown builtin: {kind}"
        console.emit("fail", f"{name}: {detail}")
        return Result(name, "fail", 2, time.monotonic() - started, detail=detail)
    duration = time.monotonic() - started
    console.emit("ok", f"{name} ({duration:.2f}s)")
    return Result(name, "pass", 0, duration)


def _record_duration(result: Result, kind: str) -> None:
    """Append one task-duration event to the durable durations log (the
    evidence base for long-class routing decisions; owner 2026-09-23).
    Never raises: logging must not gate."""
    try:
        path = ROOT / ".generated-temp" / "operator" / "task-durations.jsonl"
        path.parent.mkdir(parents=True, exist_ok=True)
        event = {
            "at": _utc_now(),
            "kind": kind,
            "task": result.name,
            "status": result.status,
            "duration_seconds": round(result.duration_seconds, 3),
            "head": _git_head_or_empty(),
        }
        with path.open("a", encoding="utf-8", newline="\n") as handle:
            handle.write(json.dumps(event, ensure_ascii=False) + "\n")
    except Exception:
        pass


def _git_head_or_empty() -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=5, check=False,
        )
        return completed.stdout.strip()[:12] if completed.returncode == 0 else ""
    except Exception:
        return ""


def _run_task(name: str, tasks: dict[str, Any], console: Console, expect_head: str | None) -> Result:
    spec = tasks.get(name)
    if not isinstance(spec, dict):
        detail = _unknown_task_error(name, tasks)
        console.emit("fail", detail)
        return Result(name, "fail", 2, detail=detail)
    if "builtin" in spec:
        result = _builtin(name, spec, console, expect_head)
    else:
        result = _run_process(name, spec, console)
    _record_duration(result, kind="task")
    return result


def _unknown_task_error(name: str, tasks: dict[str, Any]) -> str:
    available = ", ".join(sorted(tasks)) if tasks else "none"
    return f"unknown task: {name} (available: {available})"


def _receipt_path(gate: str, head: str) -> Path:
    # receipts are proof-of-PASS for an exact head, not evidence archives;
    # they live in the repo-local git-ignored temp area per the
    # generated-temp convention (tool subtree: operator/receipts)
    return ROOT / ".generated-temp" / "operator" / "receipts" / f"{gate}-{head}.json"


def _write_gate_receipt(payload: dict[str, Any]) -> None:
    if payload.get("status") != "pass":
        return
    try:
        path = _receipt_path(str(payload.get("gate")), str(payload.get("head")))
        path.parent.mkdir(parents=True, exist_ok=True)
        receipt = {
            "gate": payload.get("gate"),
            "head": payload.get("head"),
            "status": payload.get("status"),
            "timestamp": _utc_now(),
            "tasks": [
                {"name": r.get("name"), "status": r.get("status"),
                 "duration_seconds": r.get("duration_seconds")}
                for r in payload.get("results", [])
            ],
        }
        path.write_text(
            json.dumps(receipt, ensure_ascii=False, sort_keys=True, indent=1),
            encoding="utf-8",
        )
    except OSError:
        # a receipt that cannot be written degrades to no-proof-at-merge-time
        # (merge-proof then fails), which is the fail-closed direction
        pass


def _run_sequence(sequence: list[Any], tasks: dict[str, Any], console: Console, expect_head: str | None) -> list[Result]:
    results: list[Result] = []
    for stage in sequence:
        if isinstance(stage, str):
            result = _run_task(stage, tasks, console, expect_head)
            results.append(result)
            if result.returncode:
                break
            continue
        if isinstance(stage, list) and stage and all(isinstance(item, str) for item in stage):
            with concurrent.futures.ThreadPoolExecutor(max_workers=len(stage), thread_name_prefix="qiven") as pool:
                futures = {name: pool.submit(_run_task, name, tasks, console, expect_head) for name in stage}
                stage_results = [futures[name].result() for name in stage]
            results.extend(stage_results)
            if any(result.returncode for result in stage_results):
                break
            continue
        raise OperatorError(f"invalid gate stage: {stage!r}")
    return results


def _remote_repo() -> str:
    completed = _git("remote", "get-url", "origin")
    if completed.returncode:
        raise OperatorError("cannot resolve git remote 'origin'")
    value = completed.stdout.strip()
    if value.startswith("git@github.com:"):
        value = value.removeprefix("git@github.com:")
    elif "github.com/" in value:
        value = value.split("github.com/", 1)[1]
    else:
        raise OperatorError(f"origin is not a github.com repository: {value}")
    if value.endswith(".git"):
        value = value[:-4]
    if value.count("/") != 1:
        raise OperatorError(f"cannot parse GitHub repository from origin: {value}")
    return value


def _current_branch() -> str:
    completed = _git("symbolic-ref", "--quiet", "--short", "HEAD")
    branch = completed.stdout.strip()
    if completed.returncode or not branch:
        raise OperatorError("CI dispatch requires a named local branch")
    return branch


def _head() -> str:
    completed = _git("rev-parse", "HEAD")
    if completed.returncode:
        raise OperatorError("cannot resolve HEAD")
    return completed.stdout.strip()


def _remote_branch_head(branch: str) -> str:
    ref = f"refs/heads/{branch}"
    completed = _git("ls-remote", "--heads", "origin", ref)
    if completed.returncode:
        raise OperatorError(f"cannot resolve remote branch origin/{branch}")
    lines = [line.strip() for line in completed.stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        if not lines:
            raise OperatorError(f"remote branch origin/{branch} does not exist")
        raise OperatorError(f"remote branch origin/{branch} resolved ambiguously")
    parts = lines[0].split()
    if len(parts) != 2 or parts[1] != ref:
        raise OperatorError(f"unexpected ls-remote result for origin/{branch}")
    return parts[0]


def _ci_start(config: dict[str, Any], profile: str, console: Console) -> dict[str, Any]:
    ci = config.get("ci", {})
    spec = ci.get(profile) if isinstance(ci, dict) else None
    if not isinstance(spec, dict):
        raise OperatorError(f"unknown CI profile: {profile}")
    workflow = spec.get("workflow")
    inputs = spec.get("inputs", {})
    if not isinstance(workflow, str) or not workflow:
        raise OperatorError("CI profile requires workflow")
    if not isinstance(inputs, dict):
        raise OperatorError("CI profile inputs must be an object")
    if shutil.which("gh") is None:
        raise OperatorError("GitHub CLI 'gh' was not found on PATH")
    branch = _current_branch()
    head = _head()
    repo = _remote_repo()
    console.emit("run", f"ci:{profile}: verify origin/{branch} exact HEAD")
    remote_head = _remote_branch_head(branch)
    if remote_head != head:
        raise OperatorError(
            f"CI dispatch requires origin/{branch} to match local HEAD: local={head} remote={remote_head}"
        )
    console.emit("ok", f"ci:{profile}: origin/{branch} matches {head}")
    argv = ["gh", "workflow", "run", workflow, "--repo", repo, "--ref", branch]
    for key, value in inputs.items():
        argv.extend(["-f", f"{key}={value}"])
    console.emit("run", f"ci:{profile}: dispatch {workflow} on {branch}")
    completed = _run_capture(argv)
    if completed.returncode:
        console.block(completed.stdout)
        raise OperatorError(f"ci:{profile}: dispatch failed")
    console.emit("ok", f"ci:{profile}: dispatch accepted; remote job continues asynchronously")
    return {
        "profile": profile,
        "workflow": workflow,
        "repository": repo,
        "branch": branch,
        "head": head,
        "remote_head": remote_head,
        "status": "dispatched",
    }


def _common_flags() -> argparse.ArgumentParser:
    # fresh instance per parser: global flags are accepted both before and
    # after the subcommand (a repeated odyssey failure was `gate X --json`).
    # SUPPRESS defaults are the load-bearing part: a subparser parsing the
    # same flag with default=None OVERWRITES a value the top-level parser
    # already set (`--json info` silently lost the flag); with SUPPRESS an
    # absent flag sets nothing and the earlier value survives. main() reads
    # the flags with getattr fallbacks for the absent case.
    flags = argparse.ArgumentParser(add_help=False)
    flags.add_argument("--json", action="store_true", default=argparse.SUPPRESS, help="emit machine-readable JSON")
    flags.add_argument("--verbose", action="store_true", default=argparse.SUPPRESS, help="show logs for successful tasks")
    flags.add_argument("--no-color", action="store_true", default=argparse.SUPPRESS, help="disable ANSI terminal color")
    return flags


def _print_json(payload: dict[str, Any]) -> None:
    print(json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")))


LOG_SPILL_THRESHOLD = 4096


def _spill_large_logs(payload: dict[str, Any]) -> dict[str, Any]:
    # machine JSON used to be one unbounded line: buffered suite logs were
    # unrecoverable the moment it passed through a terminal filter. Beyond the
    # threshold, the full payload goes to a durable file and stdout stays a
    # compact summary carrying the log path.
    total = sum(len(str(result.get("output") or "")) for result in payload.get("results", []))
    if total <= LOG_SPILL_THRESHOLD:
        return payload
    directory = Path(tempfile.gettempdir()) / "qiven-operator"
    directory.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    name = str(payload.get("gate") or "run")
    log_file = directory / f"qiven-{name}-{stamp}-{os.getpid()}.json"
    log_file.write_text(
        json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
    )
    compact = dict(payload)
    compact["log_file"] = str(log_file)
    compact["results"] = [
        {
            key: (f"<{len(str(value))} chars; full payload in log_file>" if key == "output" and value else value)
            for key, value in result.items()
        }
        for result in payload["results"]
    ]
    return compact


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="qiven", description="Qiven local engineering operator", parents=[_common_flags()]
    )
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("info", help="show repository/operator metadata", parents=[_common_flags()])
    gate = sub.add_parser("gate", help="run the configured local validation gate", parents=[_common_flags()])
    gate.add_argument("gate", nargs="?", default=None, help="gate name; defaults to config default_gate")
    gate.add_argument("--name", dest="gate_flag", default=None, help="gate name (alternative to the positional)")
    gate.add_argument("--expect-head", default=None, help="require exact Git HEAD")
    run = sub.add_parser("run", help="run declared task(s)", parents=[_common_flags()])
    run.add_argument("tasks", nargs="+", help="task names")
    run.add_argument("--parallel", action="store_true", help="run requested tasks in parallel")
    ci = sub.add_parser("ci", help="asynchronous CI operations", parents=[_common_flags()])
    ci_sub = ci.add_subparsers(dest="ci_command", required=True)
    ci_start = ci_sub.add_parser("start", help="dispatch CI and return immediately", parents=[_common_flags()])
    ci_start.add_argument("profile", help="declared CI profile")
    exec_parser = sub.add_parser(
        "exec", help="supervised detached command execution under bounded process custody", parents=[_common_flags()]
    )
    exec_sub = exec_parser.add_subparsers(dest="exec_command", required=True)
    exec_start = exec_sub.add_parser("start", help="run a command under watchdog custody with heartbeat", parents=[_common_flags()])
    exec_start.add_argument("--timeout", type=float, default=EXEC_DEFAULT_TIMEOUT_SECONDS,
                            help="front-end supervision budget in seconds (exit 124 when it elapses; the run continues under its lease)")
    exec_start.add_argument("--max-lifetime", type=float, default=EXEC_DEFAULT_MAX_LIFETIME_SECONDS,
                            help=f"hard lease on the run's process tree in seconds (default {EXEC_DEFAULT_MAX_LIFETIME_SECONDS:.0f}, clamped to [{EXEC_MAX_LIFETIME_FLOOR_SECONDS:.0f}, {EXEC_MAX_LIFETIME_CEILING_SECONDS:.0f}])")
    exec_start.add_argument("command_args", nargs=argparse.REMAINDER, help="command after '--' to execute")
    exec_status = exec_sub.add_parser("status", help="snapshot one run: state, custody, log tail", parents=[_common_flags()])
    exec_status.add_argument("run_id", help="exec run id")
    exec_status.add_argument("--tail", type=int, default=2000, help="log tail bytes to include")
    exec_stop = exec_sub.add_parser("stop", help="terminate a run's process tree", parents=[_common_flags()])
    exec_stop.add_argument("run_id", help="exec run id")
    exec_list = exec_sub.add_parser("list", help="list known runs with state and lease", parents=[_common_flags()])
    exec_sweep = exec_sub.add_parser("sweep", help="terminate expired runs, finalize stale records", parents=[_common_flags()])
    return parser


def main(argv: list[str] | None = None) -> int:
    raw = list(sys.argv[1:]) if argv is None else list(argv)
    if raw and raw[0] == "--exec-watchdog":
        if len(raw) != 2:
            sys.stderr.write("usage: qiven_operator.py --exec-watchdog RECORD\n")
            return 2
        return _watchdog_run(raw[1])
    args = _parser().parse_args(raw)
    json_mode = bool(getattr(args, "json", False))
    verbose_mode = bool(getattr(args, "verbose", False))
    no_color_mode = bool(getattr(args, "no_color", False))
    args.json = json_mode
    args.verbose = verbose_mode
    args.no_color = no_color_mode
    console = Console(json_mode=json_mode, verbose=verbose_mode, no_color=no_color_mode)
    try:
        # dead-man insurance rides every invocation (never blocks, never
        # raises into the caller's command)
        try:
            _sweep_exec_records(console, quiet=not verbose_mode)
        except Exception:
            pass
        config = _load_config()
        if args.command == "info":
            payload = {
                "status": "ok",
                "repository": config.get("repository_name"),
                "operator_schema": config.get("schema_version"),
                "default_gate": config.get("default_gate"),
                "root": str(ROOT),
                "head": _head(),
            }
            if args.json:
                _print_json(payload)
            else:
                console.emit("ok", f"repository={payload['repository']} head={payload['head']}")
            return 0

        tasks = config.get("tasks")
        if not isinstance(tasks, dict):
            raise OperatorError("config tasks must be an object")

        if args.command == "gate":
            gate_name = args.gate or args.gate_flag or config.get("default_gate")
            gates = config.get("gates")
            if not isinstance(gates, dict) or not isinstance(gates.get(gate_name), list):
                available = ", ".join(sorted(gates)) if isinstance(gates, dict) else "none"
                raise OperatorError(f"unknown gate: {gate_name} (available: {available})")
            sequence = list(gates[gate_name])
            if args.expect_head:
                sequence.insert(0, "exact-head")
            started = time.monotonic()
            results = _run_sequence(sequence, tasks, console, args.expect_head)
            failed = [result for result in results if result.returncode]
            payload = {
                "status": "fail" if failed else "pass",
                "gate": gate_name,
                "head": _head(),
                "duration_seconds": round(time.monotonic() - started, 3),
                "results": [asdict(result) for result in results],
            }
            _write_gate_receipt(payload)
            if args.json:
                _print_json(_spill_large_logs(payload))
            else:
                console.emit("fail" if failed else "ok", f"gate:{gate_name}: {payload['status'].upper()}")
            return 1 if failed else 0

        if args.command == "run":
            started = time.monotonic()
            sequence: list[Any] = [list(args.tasks)] if args.parallel else list(args.tasks)
            results = _run_sequence(sequence, tasks, console, None)
            failed = [result for result in results if result.returncode]
            payload = {
                "status": "fail" if failed else "pass",
                "duration_seconds": round(time.monotonic() - started, 3),
                "results": [asdict(result) for result in results],
            }
            if args.json:
                _print_json(_spill_large_logs(payload))
            else:
                console.emit("fail" if failed else "ok", f"run: {payload['status'].upper()}")
            return 1 if failed else 0

        if args.command == "ci" and args.ci_command == "start":
            payload = _ci_start(config, args.profile, console)
            if args.json:
                _print_json(payload)
            elif not console.json_mode:
                console.block(
                    f"      repository:  {payload['repository']}\n"
                    f"      branch:      {payload['branch']}\n"
                    f"      head:        {payload['head']}\n"
                    f"      remote-head: {payload['remote_head']}\n"
                    f"      workflow:    {payload['workflow']}"
                )
            return 0

        if args.command == "exec":
            if args.exec_command == "start":
                command = list(args.command_args)
                if command and command[0] == "--":
                    command = command[1:]
                if not command:
                    raise OperatorError("exec start requires a command after '--'")
                payload, exit_code = _exec_start_frontend(
                    command, float(args.timeout), float(args.max_lifetime), console
                )
                if args.json:
                    _print_json(payload)
                return exit_code
            if args.exec_command == "status":
                record = _read_exec_record(args.run_id)
                snapshot = _exec_snapshot(record)
                tail = _tail_text(Path(snapshot["log"]), max(0, int(args.tail)))
                payload = dict(snapshot, status=snapshot["state"], tail=tail)
                if args.json:
                    _print_json(payload)
                else:
                    console.emit(
                        "ok" if snapshot["state"] == "done" else "wait",
                        f"exec {snapshot['id']}: {snapshot['state']}"
                        + (f", exit {snapshot['exit_code']}" if snapshot["exit_code"] is not None else "")
                        + f", log {snapshot['log_bytes']} bytes"
                        + (f", lease {snapshot['deadline_utc']}" if snapshot["deadline_utc"] else ""),
                    )
                    if tail:
                        console.block(tail)
                return 0
            if args.exec_command == "stop":
                record = _read_exec_record(args.run_id)
                payload, exit_code = _exec_stop(record, console)
                if args.json:
                    _print_json(payload)
                return exit_code
            if args.exec_command == "list":
                runs = []
                directory = _exec_dir()
                if directory.is_dir():
                    for record_path in sorted(directory.glob("*.json")):
                        record = _read_exec_record_path(record_path)
                        if record is not None:
                            runs.append(_exec_snapshot(record))
                payload = {"status": "ok", "runs": runs}
                if args.json:
                    _print_json(payload)
                else:
                    for snapshot in runs:
                        console.emit(
                            "wait" if snapshot["state"] in ("running", "reaping", "orphaned") else "ok",
                            f"exec {snapshot['id']}: {snapshot['state']}"
                            + (f", exit {snapshot['exit_code']}" if snapshot["exit_code"] is not None else "")
                            + (f", lease {snapshot['deadline_utc']}" if snapshot["deadline_utc"] else ""),
                        )
                return 0
            if args.exec_command == "sweep":
                actions = _sweep_exec_records(console, quiet=False)
                payload = {"status": "ok", "actions": actions}
                if args.json:
                    _print_json(payload)
                return 0

        raise OperatorError("unsupported command")
    except OperatorError as exc:
        if args.json:
            _print_json({"status": "error", "error": str(exc)})
        else:
            console.emit("fail", str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
