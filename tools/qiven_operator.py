from __future__ import annotations

import argparse
import concurrent.futures
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


ROOT = Path(__file__).resolve().parents[1]
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


# ---------------------------------------------------------------------------
# exec: supervised detached execution for LLM-invoked commands (the hang
# contract's mechanical answer). The child runs in its own process group,
# detached, writing to a durable log; the operator heartbeats while it
# supervises and bounds ITS OWN wait (--timeout). If the operator's caller
# dies — or the operator times out — the child keeps running; exec status
# re-attaches cheaply. An exit code no living supervisor observed is
# reported as indeterminate, never guessed.
# ---------------------------------------------------------------------------
EXEC_DEFAULT_TIMEOUT_SECONDS = 120.0
EXEC_EXIT_STILL_RUNNING = 124


def _exec_dir() -> Path:
    return ROOT / ".generated-temp" / "operator" / "exec"


def _new_exec_id() -> str:
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
    return f"{stamp}-{os.getpid():08d}-{os.urandom(3).hex()}"


def _exec_record_path(exec_id: str) -> Path:
    return _exec_dir() / f"{exec_id}.json"


def _exec_log_path(exec_id: str) -> Path:
    return _exec_dir() / f"{exec_id}.log"


def _process_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        import ctypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return False
        try:
            code = ctypes.c_ulong()
            if kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
                return code.value == STILL_ACTIVE
            return False
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _write_exec_record(record: dict[str, Any]) -> None:
    path = _exec_record_path(str(record.get("id")))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(record, ensure_ascii=False, sort_keys=True, indent=1), encoding="utf-8")


def _read_exec_record(exec_id: str) -> dict[str, Any]:
    path = _exec_record_path(exec_id)
    if not path.is_file():
        raise OperatorError(f"unknown exec id: {exec_id} (no record at {path})")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise OperatorError(f"corrupt exec record: {path}") from exc


def _exec_snapshot(record: dict[str, Any]) -> dict[str, Any]:
    pid = int(record.get("pid") or 0)
    alive = _process_alive(pid)
    log_path = Path(str(record.get("log") or ""))
    log_size = log_path.stat().st_size if log_path.is_file() else 0
    exit_code = record.get("exit_code")
    if exit_code is None:
        if record.get("status") == "stopped":
            state = "stopped"
        elif alive:
            state = "running"
        else:
            # no supervisor observed the exit: the code is genuinely unknown
            state = "indeterminate"
    else:
        state = "done"
    return {
        "id": record.get("id"),
        "pid": pid,
        "state": state,
        "exit_code": exit_code,
        "argv": record.get("argv"),
        "log": str(log_path),
        "log_bytes": log_size,
        "started_utc": record.get("started_utc"),
        "finished_utc": record.get("finished_utc"),
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


def _exec_supervise(argv: list[str], timeout_seconds: float, console: Console) -> tuple[dict[str, Any], int]:
    if not argv:
        raise OperatorError("exec requires a command after --")
    exec_id = _new_exec_id()
    log_path = _exec_log_path(exec_id)
    log_path.parent.mkdir(parents=True, exist_ok=True)

    creationflags = 0
    popen_kwargs: dict[str, Any] = {}
    if os.name == "nt":
        creationflags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.CREATE_BREAKAWAY_FROM_JOB
        popen_kwargs["creationflags"] = creationflags
    else:
        popen_kwargs["start_new_session"] = True

    started = time.monotonic()
    try:
        with log_path.open("wb") as log:
            try:
                process = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **popen_kwargs)
            except OSError as exc:
                # CREATE_BREAKAWAY_FROM_JOB fails outright when the ancestor
                # job forbids breakaway; retry without it — survival of the
                # child across CALLER death is best-effort, never a lie.
                popen_kwargs.pop("creationflags", None)
                if os.name == "nt":
                    popen_kwargs["creationflags"] = creationflags & ~subprocess.CREATE_BREAKAWAY_FROM_JOB
                try:
                    process = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **popen_kwargs)
                except OSError as second:
                    detail = f"could not start exec process: {second}"
                    console.emit("fail", detail)
                    return {"status": "error", "error": detail, "argv": argv}, 2

            record = {
                "id": exec_id,
                "argv": argv,
                "pid": process.pid,
                "log": str(log_path),
                "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "timeout_seconds": timeout_seconds,
            }
            _write_exec_record(record)

            console.emit("run", f"exec {exec_id}: pid {process.pid}, log {log_path}")
            next_heartbeat = time.monotonic() + HEARTBEAT_SECONDS
            while True:
                returncode = process.poll()
                if returncode is not None:
                    break
                now = time.monotonic()
                if now - started >= timeout_seconds:
                    snapshot = _exec_snapshot(record)
                    console.emit(
                        "wait",
                        f"exec {exec_id}: still running after {timeout_seconds:.0f}s "
                        f"(log {snapshot['log_bytes']} bytes); operator returns, child continues",
                    )
                    payload = dict(snapshot, status="still-running")
                    return payload, EXEC_EXIT_STILL_RUNNING
                if now >= next_heartbeat:
                    log_bytes = log_path.stat().st_size if log_path.is_file() else 0
                    console.emit("wait", f"exec {exec_id}: running for {now - started:.0f}s, log {log_bytes} bytes")
                    next_heartbeat = now + HEARTBEAT_SECONDS
                time.sleep(POLL_SECONDS)

            duration = time.monotonic() - started
            record["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            record["exit_code"] = returncode
            _write_exec_record(record)
            snapshot = _exec_snapshot(record)
            snapshot["duration_seconds"] = round(duration, 3)
            console.emit("ok" if returncode == 0 else "fail", f"exec {exec_id}: exit {returncode} ({duration:.2f}s)")
            payload = dict(snapshot, status="done")
            return payload, returncode
    finally:
        pass


def _exec_stop(record: dict[str, Any], console: Console) -> tuple[dict[str, Any], int]:
    pid = int(record.get("pid") or 0)
    if not _process_alive(pid):
        snapshot = _exec_snapshot(record)
        snapshot["status"] = "not-running"
        return snapshot, 0
    if os.name == "nt":
        completed = _run_capture(["taskkill", "/T", "/F", "/PID", str(pid)])
    else:
        try:
            import signal

            os.killpg(pid, signal.SIGTERM)
            completed_returncode = 0
        except OSError:
            completed_returncode = 1
        from types import SimpleNamespace

        completed = SimpleNamespace(returncode=completed_returncode, stdout="")
    if completed.returncode == 0:
        record["status"] = "stopped"
        record["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        _write_exec_record(record)
    snapshot = _exec_snapshot(record)
    snapshot["status"] = "stopped" if completed.returncode == 0 else "stop-failed"
    console.emit("ok" if completed.returncode == 0 else "fail", f"exec stop {record.get('id')}: {snapshot['status']}")
    return snapshot, 0 if completed.returncode == 0 else 1


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
    started = time.monotonic()
    console.emit("run", name)
    try:
        argv = _argv_for_task(spec)
    except OperatorError as exc:
        console.emit("fail", f"{name}: {exc}")
        return Result(name, "fail", 2, detail=str(exc))

    env = os.environ.copy()
    env.setdefault("PYTHONUTF8", "1")
    env.setdefault("PYTHONIOENCODING", "utf-8")
    with tempfile.NamedTemporaryFile(prefix="qiven-operator-", suffix=".log", delete=False) as handle:
        log_path = Path(handle.name)
    try:
        with log_path.open("w", encoding="utf-8", errors="replace") as log:
            try:
                process = subprocess.Popen(argv, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, text=True)
            except OSError as exc:
                detail = f"could not start process: {exc}"
                console.emit("fail", f"{name}: {detail}")
                return Result(name, "fail", 2, time.monotonic() - started, detail=detail)
            next_heartbeat = time.monotonic() + HEARTBEAT_SECONDS
            while True:
                returncode = process.poll()
                if returncode is not None:
                    break
                now = time.monotonic()
                if now >= next_heartbeat:
                    console.emit("wait", f"{name}: running for {now - started:.0f}s")
                    next_heartbeat = now + HEARTBEAT_SECONDS
                time.sleep(POLL_SECONDS)
        duration = time.monotonic() - started
        output = log_path.read_text(encoding="utf-8", errors="replace")
        if returncode == 0:
            console.emit("ok", f"{name} ({duration:.2f}s)")
            if console.verbose:
                console.block(output)
            return Result(name, "pass", 0, duration, output=output)
        console.emit("fail", f"{name}: exit {returncode} ({duration:.2f}s)")
        console.block(output)
        return Result(name, "fail", returncode, duration, output=output)
    finally:
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


def _run_task(name: str, tasks: dict[str, Any], console: Console, expect_head: str | None) -> Result:
    spec = tasks.get(name)
    if not isinstance(spec, dict):
        detail = _unknown_task_error(name, tasks)
        console.emit("fail", detail)
        return Result(name, "fail", 2, detail=detail)
    if "builtin" in spec:
        return _builtin(name, spec, console, expect_head)
    return _run_process(name, spec, console)


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
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "tasks": [
                {"name": r.get("name"), "status": r.get("status")}
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
        "exec", help="supervised detached command execution (hang-contract answer)", parents=[_common_flags()]
    )
    exec_sub = exec_parser.add_subparsers(dest="exec_command", required=True)
    exec_start = exec_sub.add_parser("start", help="run a command detached with heartbeat; bounded supervision", parents=[_common_flags()])
    exec_start.add_argument("--timeout", type=float, default=EXEC_DEFAULT_TIMEOUT_SECONDS, help="operator supervision ceiling in seconds (the child survives past it)")
    exec_start.add_argument("command_args", nargs=argparse.REMAINDER, help="command after '--' to execute")
    exec_status = exec_sub.add_parser("status", help="snapshot one run: state, liveness, log tail", parents=[_common_flags()])
    exec_status.add_argument("run_id", help="exec run id")
    exec_status.add_argument("--tail", type=int, default=2000, help="log tail bytes to include")
    exec_stop = exec_sub.add_parser("stop", help="terminate a run's process tree", parents=[_common_flags()])
    exec_stop.add_argument("run_id", help="exec run id")
    exec_list = exec_sub.add_parser("list", help="list known runs with state", parents=[_common_flags()])
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    json_mode = bool(getattr(args, "json", False))
    verbose_mode = bool(getattr(args, "verbose", False))
    no_color_mode = bool(getattr(args, "no_color", False))
    args.json = json_mode
    args.verbose = verbose_mode
    args.no_color = no_color_mode
    console = Console(json_mode=json_mode, verbose=verbose_mode, no_color=no_color_mode)
    try:
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
                payload, exit_code = _exec_supervise(command, float(args.timeout), console)
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
                        + f", log {snapshot['log_bytes']} bytes",
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
                        try:
                            runs.append(_exec_snapshot(json.loads(record_path.read_text(encoding="utf-8"))))
                        except (json.JSONDecodeError, OSError):
                            continue
                payload = {"status": "ok", "runs": runs}
                if args.json:
                    _print_json(payload)
                else:
                    for snapshot in runs:
                        console.emit(
                            "wait" if snapshot["state"] == "running" else "ok",
                            f"exec {snapshot['id']}: {snapshot['state']}"
                            + (f", exit {snapshot['exit_code']}" if snapshot["exit_code"] is not None else ""),
                        )
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
