"""Windows test containment. Never activates its private desktop.

Public API (stable):
    run(argv, cwd, out, timeout=90, env=None, evidence_expected=None, startup_checks=True) -> record dict
    IsolatedRun(argv, cwd, out, timeout=90, env=None, evidence_expected=None, startup_checks=True)
        .start() -> self, .poll() -> exit code or None, .terminate(), .finish() -> record dict, .close()

What a launch gets:
  - explicit argument policy: at least an exe and one test argument; no empty / whitespace-only
    element; no literal "$args" element (the PowerShell automatic-variable bug of chain12.ps1);
  - startup checks before any process exists: the exe exists (sha256/mtime/size recorded), the cwd
    exists, a Data junction resolves to a directory holding Base.rte, Userdata is writable (probe
    file written and deleted), the out dir is private (not under the game's Data) and writable;
  - a private desktop that is never switched to, CCCP_HEADLESS=1, stdin from NUL, stdout+stderr to
    <out>/stdout.log, a job object with kill-on-close so descendants die with the run or the runner;
  - a deadline: on timeout the whole job is terminated and the exit code reads 124;
  - <out>/launch.json with argv verbatim, env keys set, desktop names before/after, pid, exit code,
    elapsed, timed_out, stdout size, engine verdict lines, and the expected-vs-present evidence list.
Nothing here ever kills a process by name.
"""

import ctypes as C
from ctypes import wintypes as W
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import time
import uuid
from pathlib import Path

K = C.WinDLL("kernel32", use_last_error=True)
U = C.WinDLL("user32", use_last_error=True)
SIZE_T = C.c_size_t


class SA(C.Structure):
    _fields_ = [
        ("nLength", W.DWORD),
        ("lpSecurityDescriptor", W.LPVOID),
        ("bInheritHandle", W.BOOL),
    ]


class SI(C.Structure):
    _fields_ = [
        ("cb", W.DWORD),
        ("lpReserved", W.LPWSTR),
        ("lpDesktop", W.LPWSTR),
        ("lpTitle", W.LPWSTR),
        ("dwX", W.DWORD),
        ("dwY", W.DWORD),
        ("dwXSize", W.DWORD),
        ("dwYSize", W.DWORD),
        ("dwXCountChars", W.DWORD),
        ("dwYCountChars", W.DWORD),
        ("dwFillAttribute", W.DWORD),
        ("dwFlags", W.DWORD),
        ("wShowWindow", W.WORD),
        ("cbReserved2", W.WORD),
        ("lpReserved2", C.POINTER(W.BYTE)),
        ("hStdInput", W.HANDLE),
        ("hStdOutput", W.HANDLE),
        ("hStdError", W.HANDLE),
    ]


class PI(C.Structure):
    _fields_ = [
        ("hProcess", W.HANDLE),
        ("hThread", W.HANDLE),
        ("dwProcessId", W.DWORD),
        ("dwThreadId", W.DWORD),
    ]


class BASIC(C.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", C.c_int64),
        ("PerJobUserTimeLimit", C.c_int64),
        ("LimitFlags", W.DWORD),
        ("MinimumWorkingSetSize", SIZE_T),
        ("MaximumWorkingSetSize", SIZE_T),
        ("ActiveProcessLimit", W.DWORD),
        ("Affinity", SIZE_T),
        ("PriorityClass", W.DWORD),
        ("SchedulingClass", W.DWORD),
    ]


class IO(C.Structure):
    _fields_ = [
        (s, C.c_uint64)
        for s in [
            "ReadOperationCount",
            "WriteOperationCount",
            "OtherOperationCount",
            "ReadTransferCount",
            "WriteTransferCount",
            "OtherTransferCount",
        ]
    ]


class EXT(C.Structure):
    _fields_ = [
        ("BasicLimitInformation", BASIC),
        ("IoInfo", IO),
        ("ProcessMemoryLimit", SIZE_T),
        ("JobMemoryLimit", SIZE_T),
        ("PeakProcessMemoryUsed", SIZE_T),
        ("PeakJobMemoryUsed", SIZE_T),
    ]


def api(dll, name, args, result):
    fn = getattr(dll, name)
    fn.argtypes = args
    fn.restype = result
    return fn


create_desktop = api(
    U,
    "CreateDesktopW",
    [W.LPCWSTR, W.LPCWSTR, W.LPVOID, W.DWORD, W.DWORD, C.POINTER(SA)],
    W.HANDLE,
)
close_desktop = api(U, "CloseDesktop", [W.HANDLE], W.BOOL)
open_input = api(U, "OpenInputDesktop", [W.DWORD, W.BOOL, W.DWORD], W.HANDLE)
get_object = api(
    U,
    "GetUserObjectInformationW",
    [W.HANDLE, C.c_int, W.LPVOID, W.DWORD, C.POINTER(W.DWORD)],
    W.BOOL,
)
create_job = api(K, "CreateJobObjectW", [C.POINTER(SA), W.LPCWSTR], W.HANDLE)
set_job = api(
    K, "SetInformationJobObject", [W.HANDLE, C.c_int, W.LPVOID, W.DWORD], W.BOOL
)
assign_job = api(K, "AssignProcessToJobObject", [W.HANDLE, W.HANDLE], W.BOOL)
terminate_job = api(K, "TerminateJobObject", [W.HANDLE, W.UINT], W.BOOL)
close_handle = api(K, "CloseHandle", [W.HANDLE], W.BOOL)
create_file = api(
    K,
    "CreateFileW",
    [W.LPCWSTR, W.DWORD, W.DWORD, C.POINTER(SA), W.DWORD, W.DWORD, W.HANDLE],
    W.HANDLE,
)
create_process = api(
    K,
    "CreateProcessW",
    [
        W.LPCWSTR,
        W.LPWSTR,
        C.POINTER(SA),
        C.POINTER(SA),
        W.BOOL,
        W.DWORD,
        W.LPVOID,
        W.LPCWSTR,
        C.POINTER(SI),
        C.POINTER(PI),
    ],
    W.BOOL,
)
resume = api(K, "ResumeThread", [W.HANDLE], W.DWORD)
wait = api(K, "WaitForSingleObject", [W.HANDLE, W.DWORD], W.DWORD)
get_exit = api(K, "GetExitCodeProcess", [W.HANDLE, C.POINTER(W.DWORD)], W.BOOL)
terminate_process = api(K, "TerminateProcess", [W.HANDLE, W.UINT], W.BOOL)

INVALID_HANDLE = C.c_void_p(-1).value
VERDICT_LINE = re.compile(
    r"^\[[^\]]+\].*\b(PASS|FAIL|FAILED|complete|completed|finished|failed|refused)\b"
)


class ArgumentPolicyError(ValueError):
    """The argument vector is not an explicit, well-formed test launch."""


JOB_MEMORY_LIMIT_BYTES = int(os.environ.get("CC_RUNNER_JOB_MEMORY_GB", "8")) * 1024 ** 3

class StartupCheckError(RuntimeError):
    """A pre-launch check failed; no process was created."""

# The private desktop keeps windows and input away from the user, but a new game process still changes the
# display's cooperative state, which minimises a fullscreen exclusive application the user is running.
# SHQueryUserNotificationState reports exactly that condition, so no game launches while it holds.
_shell32 = C.WinDLL("shell32", use_last_error=True)
_query_notification_state = _shell32.SHQueryUserNotificationState
_query_notification_state.argtypes = [C.POINTER(C.c_int)]
_query_notification_state.restype = C.HRESULT
_FULLSCREEN_STATES = {2: "QUNS_BUSY", 3: "QUNS_RUNNING_D3D_FULL_SCREEN", 4: "QUNS_PRESENTATION_MODE"}


def user_fullscreen_state():
    """The name of the user's fullscreen/presentation state, or None when a launch would not disturb the user."""
    state = C.c_int(0)
    try:
        _query_notification_state(C.byref(state))
    except OSError:
        return None
    return _FULLSCREEN_STATES.get(state.value)


def wait_while_user_fullscreen(record, save, limit_seconds=4 * 3600, poll_seconds=15):
    """Block while the user is in a fullscreen application; record the wait; refuse after the bound."""
    if os.environ.get("CC_RUNNER_IGNORE_FULLSCREEN") == "1":
        return
    waited = 0
    state = user_fullscreen_state()
    while state:
        record["waiting_for_user_fullscreen"] = state
        record["waited_for_user_fullscreen_seconds"] = waited
        save()
        if waited >= limit_seconds:
            raise StartupCheckError(f"user in {state} for {waited}s; the game is not launched over a fullscreen application")
        time.sleep(poll_seconds)
        waited += poll_seconds
        state = user_fullscreen_state()
    if waited:
        record["waited_for_user_fullscreen_seconds"] = waited
        save()



def check(value):
    if not value:
        raise C.WinError(C.get_last_error())
    return value


def object_name(handle):
    buf = C.create_unicode_buffer(512)
    needed = W.DWORD()
    check(get_object(handle, 2, buf, C.sizeof(buf), C.byref(needed)))
    return buf.value


def input_desktop_name():
    h = check(open_input(0, False, 1))
    try:
        return object_name(h)
    finally:
        close_desktop(h)


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha256_of(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def validate_argv(argv):
    if argv is None or len(argv) < 2:
        raise ArgumentPolicyError(
            "Explicit test arguments are required; bare launches are rejected."
        )
    for index, item in enumerate(argv):
        if not isinstance(item, (str, os.PathLike)):
            raise ArgumentPolicyError(f"argv[{index}] is not a string: {item!r}")
        text = str(item)
        if not text.strip():
            raise ArgumentPolicyError(
                f"argv[{index}] is empty or whitespace; every element must be an explicit argument."
            )
        if "$args" in text:
            raise ArgumentPolicyError(
                f'argv[{index}] contains the literal "$args": a PowerShell automatic variable leaked instead of the test arguments.'
            )
    return [str(a) for a in argv]


def _probe_writable(directory):
    probe = directory / f".audit_write_probe_{uuid.uuid4().hex}"
    probe.write_text("probe", encoding="utf-8")
    ok = probe.read_text(encoding="utf-8") == "probe"
    probe.unlink()
    return ok


class IsolatedRun:
    def __init__(
        self,
        argv,
        cwd,
        out,
        timeout=90,
        env=None,
        evidence_expected=None,
        startup_checks=True,
    ):
        self.argv = validate_argv(argv)
        self.cwd = Path(cwd).resolve()
        self.out = Path(out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.timeout = timeout
        self.desktop = None
        self.job = None
        self.process = None
        self.thread = None
        self.start_time = None
        self.do_startup_checks = startup_checks
        self.evidence_expected = [
            str(Path(p) if Path(p).is_absolute() else self.out / p)
            for p in (evidence_expected or [])
        ]
        self.before = input_desktop_name()
        self.name = "CortexTest_" + uuid.uuid4().hex
        extra = dict(env or {})
        self.env = dict(os.environ)
        self.env.update(extra)
        self.env["CCCP_HEADLESS"] = "1"
        env_set = dict(extra)
        env_set["CCCP_HEADLESS"] = "1"
        self.record = {
            "runner": "win32_test_runner.py",
            "runner_pid": os.getpid(),
            "argv": self.argv,
            "cwd": str(self.cwd),
            "timeout_seconds": timeout,
            "private_desktop": self.name,
            "input_desktop_before": self.before,
            "created_utc": utc_now(),
            "headless_env": "1",
            "env_set": env_set,
            "desktop_switch_restricted": True,
            "job_kill_on_close": True,
            "started": False,
            "startup_checks": [],
            "evidence_expected": self.evidence_expected,
        }
        self._save()

    def _check(self, name, ok, detail):
        self.record["startup_checks"].append(
            {"check": name, "ok": bool(ok), "detail": detail}
        )
        return ok

    def _startup_checks(self):
        exe = Path(self.argv[0])
        exe_ok = exe.is_file()
        self._check("exe_exists", exe_ok, str(exe))
        if exe_ok:
            st = exe.stat()
            self.record["exe_path"] = str(exe)
            self.record["exe_bytes"] = st.st_size
            self.record["exe_mtime_utc"] = datetime.datetime.fromtimestamp(
                st.st_mtime, datetime.timezone.utc
            ).isoformat()
            self.record["exe_sha256"] = sha256_of(exe)
            self._check("exe_sha256_recorded", True, self.record["exe_sha256"])
        self._check("cwd_exists", self.cwd.is_dir(), str(self.cwd))
        data = self.cwd / "Data"
        # The Data/Userdata checks describe a game runtime; a stand-in child (python) has neither.
        game_runtime = exe.name.lower().startswith("cortex command")
        self.record["game_runtime_checks"] = game_runtime
        if game_runtime:
            if data.exists():
                target = Path(os.path.realpath(data))
                self._check(
                    "data_junction_resolves",
                    target.is_dir() and (target / "Base.rte").exists(),
                    f"{data} -> {target}",
                )
            else:
                self._check("data_junction_resolves", False, f"{data} missing")
            userdata = self.cwd / "Userdata"
            try:
                self._check(
                    "userdata_writable",
                    userdata.is_dir() and _probe_writable(userdata),
                    str(userdata),
                )
            except OSError as exc:
                self._check("userdata_writable", False, f"{userdata}: {exc}")
        try:
            private = (
                not str(self.out)
                .lower()
                .startswith(
                    str(Path(os.path.realpath(data)) if data.exists() else data).lower()
                )
            )
            self._check(
                "out_dir_private_writable",
                private and _probe_writable(self.out),
                str(self.out),
            )
        except OSError as exc:
            self._check("out_dir_private_writable", False, f"{self.out}: {exc}")
        failed = [c for c in self.record["startup_checks"] if not c["ok"]]
        self._save()
        if failed:
            raise StartupCheckError(
                "; ".join(f"{c['check']}: {c['detail']}" for c in failed)
            )

    def start(self):
        handles = []
        try:
            if self.do_startup_checks:
                self._startup_checks()
            wait_while_user_fullscreen(self.record, self._save)
            self.desktop = check(create_desktop(self.name, None, None, 0, 0x01FF, None))
            self.job = check(create_job(None, None))
            lim = EXT()
            # KILL_ON_JOB_CLOSE, plus a job-wide commit limit: a runaway test process (one lockstep selftest
            # reached 26 GB on 2026-09-09) fails its own allocations instead of taking the machine down.
            lim.BasicLimitInformation.LimitFlags = 0x2000 | 0x200
            lim.JobMemoryLimit = JOB_MEMORY_LIMIT_BYTES
            self.record["job_memory_limit_bytes"] = JOB_MEMORY_LIMIT_BYTES
            check(set_job(self.job, 9, C.byref(lim), C.sizeof(lim)))
            ui = W.DWORD(0x40 | 0x10)
            check(set_job(self.job, 4, C.byref(ui), C.sizeof(ui)))
            sa = SA(C.sizeof(SA), None, True)
            out_handle = create_file(
                str(self.out / "stdout.log"), 0x40000000, 3, C.byref(sa), 2, 0x80, None
            )
            if out_handle in (None, INVALID_HANDLE):
                raise C.WinError(C.get_last_error())
            handles.append(out_handle)
            in_handle = create_file("NUL", 0x80000000, 3, C.byref(sa), 3, 0x80, None)
            if in_handle in (None, INVALID_HANDLE):
                raise C.WinError(C.get_last_error())
            handles.append(in_handle)
            si = SI()
            si.cb = C.sizeof(SI)
            si.lpDesktop = "winsta0\\" + self.name
            si.dwFlags = 0x101
            si.wShowWindow = 0
            si.hStdInput = in_handle
            si.hStdOutput = out_handle
            si.hStdError = out_handle
            pi = PI()
            cmd = C.create_unicode_buffer(subprocess.list2cmdline(self.argv))
            envbuf = C.create_unicode_buffer(
                "\0".join(
                    f"{k}={v}"
                    for k, v in sorted(self.env.items(), key=lambda kv: kv[0].upper())
                )
                + "\0\0"
            )
            check(
                create_process(
                    self.argv[0],
                    cmd,
                    None,
                    None,
                    True,
                    0x00000004 | 0x08000000 | 0x00000400,
                    envbuf,
                    str(self.cwd),
                    C.byref(si),
                    C.byref(pi),
                )
            )
            self.process = pi.hProcess
            self.thread = pi.hThread
            self.record["pid"] = pi.dwProcessId
            check(assign_job(self.job, self.process))
            if resume(self.thread) == 0xFFFFFFFF:
                raise C.WinError(C.get_last_error())
            close_handle(self.thread)
            self.thread = None
            self.start_time = time.monotonic()
            self.record["started"] = True
            self.record["started_utc"] = utc_now()
            self._save()
            return self
        except BaseException:
            self.close()
            raise
        finally:
            for h in handles:
                close_handle(h)

    def _save(self):
        (self.out / "launch.json").write_text(
            json.dumps(self.record, indent=2), encoding="utf-8"
        )

    def poll(self):
        state = wait(self.process, 0)
        if state == 0:
            code = W.DWORD()
            check(get_exit(self.process, C.byref(code)))
            return code.value
        if time.monotonic() - self.start_time >= self.timeout:
            self.record["timed_out"] = True
            check(terminate_job(self.job, 124))
            wait(self.process, 5000)
            return 124
        return None

    def _collect_evidence(self):
        log = self.out / "stdout.log"
        self.record["stdout_bytes"] = log.stat().st_size if log.exists() else 0
        verdicts = []
        if log.exists():
            for line in log.read_text(
                encoding="utf-8-sig", errors="replace"
            ).splitlines():
                if VERDICT_LINE.search(line):
                    verdicts.append(line.strip()[:400])
        self.record["verdict_lines"] = verdicts[:100] + verdicts[100:][-100:]
        self.record["verdict_count"] = len(verdicts)
        self.record["last_verdict"] = verdicts[-1] if verdicts else None
        self.record["verdict_present"] = bool(verdicts)
        present = [
            p
            for p in self.evidence_expected
            if Path(p).is_file() and Path(p).stat().st_size > 0
        ]
        missing = [p for p in self.evidence_expected if p not in present]
        self.record["evidence_present"] = present
        self.record["evidence_missing"] = missing
        self.record["evidence_complete"] = not missing

    def terminate(self, code=137, reason="injected process drop"):
        if not self.process or self.poll() is not None:
            raise RuntimeError("the test process is not running")
        self.record["injected_termination"] = reason
        check(terminate_job(self.job, code))
        self._save()

    def finish(self):
        code = None
        while code is None:
            code = self.poll()
            if code is None:
                time.sleep(0.1)
        self.record["exit_code"] = code
        self.record["elapsed_seconds"] = round(time.monotonic() - self.start_time, 3)
        self.record["ended_utc"] = utc_now()
        self.record["input_desktop_after"] = input_desktop_name()
        self.record.setdefault("timed_out", False)
        self.close()
        self._collect_evidence()
        self._save()
        return self.record

    def close(self):
        if self.thread:
            close_handle(self.thread)
            self.thread = None
        if self.job:
            close_handle(self.job)
            self.job = None
        if self.process:
            if wait(self.process, 1000) != 0:
                terminate_process(self.process, 125)
                wait(self.process, 5000)
            close_handle(self.process)
            self.process = None
        if self.desktop:
            close_desktop(self.desktop)
            self.desktop = None
        self.record["job_closed"] = True
        self._save()


def run(
    argv, cwd, out, timeout=90, env=None, evidence_expected=None, startup_checks=True
):
    obj = IsolatedRun(argv, cwd, out, timeout, env, evidence_expected, startup_checks)
    try:
        return obj.start().finish()
    finally:
        obj.close()


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--cwd", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--expect", action="append", default=[])
    ap.add_argument("--no-startup-checks", action="store_true")
    ap.add_argument("argv", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    argv = a.argv[1:] if a.argv and a.argv[0] == "--" else a.argv
    try:
        record = run(
            argv,
            a.cwd,
            a.out,
            a.timeout,
            evidence_expected=a.expect,
            startup_checks=not a.no_startup_checks,
        )
    except (ArgumentPolicyError, StartupCheckError) as exc:
        print(json.dumps({"refused": type(exc).__name__, "reason": str(exc)}, indent=2))
        sys.exit(3)
    print(json.dumps(record, indent=2))
    sys.exit(
        0
        if record.get("exit_code") == 0 and record.get("evidence_complete", True)
        else 1
    )
