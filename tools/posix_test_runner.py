"""POSIX IsolatedRun. No firewall check on POSIX."""

from __future__ import annotations

import datetime
import hashlib
import json
import os
import re
import signal
import subprocess
import sys
import time
import uuid
from pathlib import Path
from typing import Any, Mapping, Sequence

VERDICT_LINE = re.compile(
    r"^\[[^\]]+\].*\b(PASS|FAIL|FAILED|complete|completed|finished|failed|refused)\b"
)

SETTINGS_OVERRIDES = {
    "MuteMaster": "1",
    "MuteMusic": "1",
    "MuteSounds": "1",
    "MasterVolume": "0",
    "MusicVolume": "0",
    "SoundVolume": "0",
    "Fullscreen": "0",
    "SkipIntro": "1",
    "EnableVSync": "0",
    "ResolutionX": "960",
    "ResolutionY": "540",
    "UseMultiDisplays": "0",
    "LaunchIntoActivity": "0",
}

DEFAULT_SETTINGS = """SettingsMan
////////////////////////////////////////////////////////////////////////
// Display Settings

	PaletteFile = ContentFile
		FilePath = Data/Base.rte/palette.bmp
		IsMemoryPNG = 0
	ResolutionX = 960
	ResolutionY = 540
	ResolutionMultiplier = 1.000000
	Fullscreen = 0
	EnableVSync = 1
	UseMultiDisplays = 0
	TwoPlayerSplitscreenVertSplit = 0


////////////////////////////////////////////////////////////////////////
// Audio Settings

	MasterVolume = 50.000000
	MuteMaster = 0
	MusicVolume = 100.000000
	MuteMusic = 0
	SoundVolume = 100.000000
	MuteSounds = 0
	MuteAudioOnFocusLoss = 0
	SoundPanningEffectStrength = 0.500000
	ListenerZOffset = 400.000000
	MinimumDistanceForPanning = 30.000000


////////////////////////////////////////////////////////////////////////
// Gameplay Settings

	ShowForeignItems = 1
	FlashOnBrainDamage = 1
	BlipOnRevealUnseen = 0
	MaxUnheldItems = 100
	UnheldItemsHUDDisplayRange = 500.000000
	AlwaysDisplayUnheldItemsInStrategicMode = 1
	SubPieMenuHoverOpenDelay = 1000
	EndlessMetaGameMode = 0
	EnableCrabBombs = 0
	CrabBombThreshold = 42
	ShowEnemyHUD = 1
	SmartBuyMenuNavigation = 1
	ScrapCompactingHeight = 25
	AutomaticGoldDeposit = 1


////////////////////////////////////////////////////////////////////////
// Screen Shake Settings

	ScreenShakeStrength = 1.000000
	ScreenShakeDecay = 50.000000
	MaxScreenShakeTime = 1.000000
	DefaultShakePerUnitOfGibEnergy = 0.001000
	DefaultShakePerUnitOfRecoilEnergy = 0.500000
	DefaultShakeFromRecoilMaximum = 0.000000


////////////////////////////////////////////////////////////////////////
// Default Activity Settings

	LaunchIntoActivity = 0
	DefaultActivityType = GATutorial
	DefaultActivityName = Tutorial Mission
	DefaultSceneName = Tutorial Bunker


////////////////////////////////////////////////////////////////////////
// Engine Settings

	DisableLuaJIT = 0
	EnableLuaDebugging = 0
	RecommendedMOIDCount = 512
	SceneBackgroundAutoScaleMode = 1
	DisableFactionBuyMenuThemes = 0
	DisableFactionBuyMenuThemeCursors = 0
	PathFinderGridNodeSize = 24
	AIUpdateInterval = 2
	NumberOfLuaStatesOverride = -1
	ForceImmediatePathingRequestCompletion = 0
	EnableParticleSettling = 1
	EnableMOSubtraction = 1
	DeltaTime = 0.016667


////////////////////////////////////////////////////////////////////////
// Editor Settings

	AllowSavingToBase = 0
	ShowMetaScenes = 0


////////////////////////////////////////////////////////////////////////
// Misc Settings

	SkipIntro = 0
	ShowToolTips = 1
	CaseSensitiveFilePaths = 1
	DisableLoadingScreenProgressReport = 1
	LoadingScreenProgressReportPrecision = 100
	ConsoleScreenRatio = 0.300000
	ConsoleUseMonospaceFont = 0
	AdvancedPerformanceStats = 1
	MenuTransitionDurationMultiplier = 1.000000


////////////////////////////////////////////////////////////////////////
// Modder Debug Settings

	DrawAtomGroupVisualizations = 0
	DrawHandAndFootGroupVisualizations = 0
	DrawLimbPathVisualizations = 0
	DrawRaycastVisualizations = 0
	DrawPixelCheckVisualizations = 0
	PrintDebugInfo = 0
	MeasureModuleLoadTime = 0


////////////////////////////////////////////////////////////////////////
// Input Mapping

	ForceDisableMultimouse = 0
	MouseSensitivity = 0.600000

	// Input Devices:  0 = Keyboard Only, 1 = Mouse + Keyboard, 2 = Gamepad One, 3 = Gamepad Two, , 4 = Gamepad Three, 5 = Gamepad Four
	// Scheme Presets: 0 = No Preset, 1 = Arrow Keys, 2 = WASD Keys, 3 = Mouse + WASD Keys, 4 = Generic DPad, 5 = Generic Dual Analog, 6 = SNES, 7 = DualShock 4, 8 = XBox 360


////////////////////////////////////////////////////////////////////////
// Player 1

	Player1Scheme = InputScheme
		Device = 1
		Preset = 3
		JoystickDeadzoneType = 0
		JoystickDeadzone = 0.010000
		DigitalAimSpeed = 1.000000


////////////////////////////////////////////////////////////////////////
// Player 2

	Player2Scheme = InputScheme
		Device = 0
		Preset = 1
		JoystickDeadzoneType = 0
		JoystickDeadzone = 0.010000
		DigitalAimSpeed = 1.000000


////////////////////////////////////////////////////////////////////////
// Player 3

	Player3Scheme = InputScheme
		Device = 2
		Preset = 8
		JoystickDeadzoneType = 0
		JoystickDeadzone = 0.010000
		DigitalAimSpeed = 1.000000


////////////////////////////////////////////////////////////////////////
// Player 4

	Player4Scheme = InputScheme
		Device = 3
		Preset = 8
		JoystickDeadzoneType = 0
		JoystickDeadzone = 0.010000
		DigitalAimSpeed = 1.000000
"""


class ArgumentPolicyError(ValueError):
    """The argument vector is not an explicit, well-formed test launch."""


class StartupCheckError(RuntimeError):
    """A pre-launch check failed; no process was created."""


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha256_of(path: Path) -> str:
    with path.open("rb") as handle:
        digest = getattr(hashlib, "file_digest", None)
        if digest is not None:
            return digest(handle, "sha256").hexdigest()
        hasher = hashlib.sha256()
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            hasher.update(chunk)
        return hasher.hexdigest()


def validate_argv(argv: Sequence[str | os.PathLike[str]] | None) -> list[str]:
    if argv is None or len(argv) < 2:
        raise ArgumentPolicyError(
            "Explicit test arguments are required; bare launches are rejected."
        )
    out: list[str] = []
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
        out.append(text)
    return out


def _probe_writable(directory: Path) -> bool:
    probe = directory / f".audit_write_probe_{uuid.uuid4().hex}"
    probe.write_text("probe", encoding="utf-8")
    ok = probe.read_text(encoding="utf-8") == "probe"
    probe.unlink()
    return ok


def resolve_binary(repo: Path | str, binary: Path | str | None = None) -> Path:
    if binary is not None:
        return Path(binary).resolve()
    env = os.environ.get("CCCP_TEST_BINARY")
    if env:
        return Path(env).resolve()
    return Path(repo).resolve() / "build-gns" / "CortexCommand"


def resolve_data(repo: Path | str, data: Path | str | None = None) -> Path:
    if data is not None:
        return Path(data).resolve()
    env = os.environ.get("CCCP_TEST_DATA")
    if env:
        return Path(env).resolve()
    return Path(repo).resolve() / "Data"


def load_settings_template(repo: Path) -> str:
    env = os.environ.get("CCCP_TEST_SETTINGS")
    if env:
        path = Path(env)
        if path.is_file():
            return path.read_text(encoding="utf-8-sig")
    repo_settings = repo / "Userdata" / "Settings.ini"
    if repo_settings.is_file():
        return repo_settings.read_text(encoding="utf-8-sig")
    return DEFAULT_SETTINGS


def apply_settings_overrides(text: str, values: Mapping[str, str]) -> str:
    settings = text
    for name, value in values.items():
        pattern = rf"(?m)^(\s*{re.escape(name)}\s*=\s*)[^\r\n]*"
        settings, count = re.subn(
            pattern, lambda match, replacement=value: match[1] + replacement, settings
        )
        if count == 0:
            settings += f"\n\t{name} = {value}\n"
    return settings


def dylib_search_path(repo: Path, exe: Path) -> str | None:
    dirs: list[str] = []
    for candidate in (
        repo / "external" / "lib" / "macos",
        exe.resolve().parent.parent / "external" / "lib" / "macos",
        exe.resolve().parent,
    ):
        if candidate.is_dir():
            text = str(candidate)
            if text not in dirs:
                dirs.append(text)
    existing = os.environ.get("DYLD_LIBRARY_PATH")
    if existing:
        dirs.append(existing)
    return os.pathsep.join(dirs) if dirs else None


def with_headless(args: Sequence[Any]) -> list[str]:
    texts = [str(item) for item in args]
    if "-headless" not in texts:
        return ["-headless", *texts]
    return texts


def is_game_binary(exe: Path) -> bool:
    name = exe.name.lower()
    return name.startswith("cortex command") or name == "cortexcommand"


def prepare_runtime(
    repo: Path | str,
    out: Path | str,
    data: Path | str | None = None,
    binary: Path | str | None = None,
) -> Path:
    repo_path = Path(repo).resolve()
    out_path = Path(out).resolve()
    runtime = out_path / "runtime"
    runtime.mkdir(parents=True, exist_ok=False)
    for name in ("Mods", "ScreenShots", "Userdata", "Temp"):
        (runtime / name).mkdir()
    data_path = resolve_data(repo_path, data)
    (runtime / "Data").symlink_to(data_path, target_is_directory=True)
    settings = apply_settings_overrides(load_settings_template(repo_path), SETTINGS_OVERRIDES)
    (runtime / "Userdata" / "Settings.ini").write_text(settings, encoding="utf-8")
    exe = resolve_binary(repo_path, binary)
    manifest = {
        "executable": str(exe),
        "cwd": str(runtime),
        "data": str(data_path),
        "settings_sha256": hashlib.sha256(settings.encode()).hexdigest(),
        "settings_overrides": SETTINGS_OVERRIDES,
    }
    (out_path / "runtime.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return runtime


def posix_launch_env(repo: Path, runtime: Path, exe: Path, extra: Mapping[str, str] | None) -> dict[str, str]:
    env: dict[str, str] = dict(extra or {})
    temp = str(runtime / "Temp")
    env.update(TEMP=temp, TMP=temp, TMPDIR=temp)
    if sys.platform == "darwin":
        env.setdefault("SDL_MAC_BACKGROUND_APP", "1")
        env.setdefault("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1")
        search = dylib_search_path(repo, exe)
        if search:
            env.setdefault("DYLD_LIBRARY_PATH", search)
    return env


def make_run(
    repo: Path | str,
    args: Sequence[Any],
    out: Path | str,
    timeout: float = 120,
    env: Mapping[str, str] | None = None,
    expected: Sequence[Path | str] | None = None,
    binary: Path | str | None = None,
    data: Path | str | None = None,
) -> IsolatedRun:
    repo_path = Path(repo).resolve()
    out_path = Path(out).resolve()
    out_path.mkdir(parents=True, exist_ok=False)
    runtime = prepare_runtime(repo_path, out_path, data=data, binary=binary)
    exe = resolve_binary(repo_path, binary)
    argv = [str(exe), *with_headless(args)]
    private_env = posix_launch_env(repo_path, runtime, exe, env)
    return IsolatedRun(argv, runtime, out_path, timeout, env=private_env, evidence_expected=expected)


class IsolatedRun:
    def __init__(
        self,
        argv: Sequence[str | os.PathLike[str]],
        cwd: Path | str,
        out: Path | str,
        timeout: float = 90,
        env: Mapping[str, str] | None = None,
        evidence_expected: Sequence[Path | str] | None = None,
        startup_checks: bool = True,
    ) -> None:
        self.argv = validate_argv(argv)
        self.cwd = Path(cwd).resolve()
        self.out = Path(out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.timeout = timeout
        self.do_startup_checks = startup_checks
        self.evidence_expected = [
            str(Path(p) if Path(p).is_absolute() else self.out / p)
            for p in (evidence_expected or [])
        ]
        extra = dict(env or {})
        self.env = dict(os.environ)
        self.env.update(extra)
        self.env["CCCP_HEADLESS"] = "1"
        env_set = dict(extra)
        env_set["CCCP_HEADLESS"] = "1"
        # A FATAL writes the minidump at this cwd-relative path, beside AbortCode.txt.
        if "CC_TEST_CRASH_DUMP" not in extra:
            self.env.setdefault("CC_TEST_CRASH_DUMP", "1")
            env_set["CC_TEST_CRASH_DUMP"] = self.env["CC_TEST_CRASH_DUMP"]
        self._proc: subprocess.Popen[bytes] | None = None
        self._stdout: Any = None
        self._stderr: Any = None
        self.start_time: float | None = None
        self.record: dict[str, Any] = {
            "runner": "posix_test_runner.py",
            "runner_pid": os.getpid(),
            "platform": sys.platform,
            "argv": self.argv,
            "cwd": str(self.cwd),
            "timeout_seconds": timeout,
            "private_desktop": "n/a",
            "input_desktop_before": "n/a",
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

    def _check(self, name: str, ok: bool, detail: str) -> bool:
        self.record["startup_checks"].append(
            {"check": name, "ok": bool(ok), "detail": detail}
        )
        return ok

    def _hash_settings(self) -> str | None:
        settings = self.cwd / "Userdata" / "Settings.ini"
        if not settings.is_file():
            return None
        return sha256_of(settings)

    def _startup_checks(self) -> None:
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
        game_runtime = is_game_binary(exe)
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
            data_root = Path(os.path.realpath(data)) if data.exists() else data
            private = not str(self.out).startswith(str(data_root))
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

    def start(self) -> IsolatedRun:
        try:
            if self.do_startup_checks:
                self._startup_checks()
            before = self._hash_settings()
            if before is not None:
                self.record["settings_sha256_before"] = before
            stdout_path = self.out / "stdout.log"
            stderr_path = self.out / "stderr.log"
            self._stdout = stdout_path.open("wb", buffering=0)
            self._stderr = stderr_path.open("wb", buffering=0)
            self._proc = subprocess.Popen(
                self.argv,
                cwd=str(self.cwd),
                env=self.env,
                stdin=subprocess.DEVNULL,
                stdout=self._stdout,
                stderr=self._stderr,
                start_new_session=True,
            )
            self.start_time = time.monotonic()
            self.record["pid"] = self._proc.pid
            self.record["started"] = True
            self.record["started_utc"] = utc_now()
            self._save()
            return self
        except BaseException:
            self.close()
            raise

    def _save(self) -> None:
        (self.out / "launch.json").write_text(
            json.dumps(self.record, indent=2), encoding="utf-8"
        )

    def _signal_group(self, sig: int) -> None:
        if self._proc is None or self._proc.pid is None:
            return
        try:
            os.killpg(self._proc.pid, sig)
        except ProcessLookupError:
            try:
                self._proc.send_signal(sig)
            except ProcessLookupError:
                return

    def poll(self) -> int | None:
        if self._proc is None:
            return self.record.get("exit_code")
        code = self._proc.poll()
        if code is not None:
            return code
        if self.start_time is not None and time.monotonic() - self.start_time >= self.timeout:
            self.record["timed_out"] = True
            self._signal_group(signal.SIGTERM)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if self._proc.poll() is not None:
                    break
                time.sleep(0.05)
            if self._proc.poll() is None:
                self._signal_group(signal.SIGKILL)
                try:
                    self._proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
            return 124
        return None

    def _collect_evidence(self) -> None:
        log = self.out / "stdout.log"
        err = self.out / "stderr.log"
        self.record["stdout_bytes"] = log.stat().st_size if log.exists() else 0
        self.record["stderr_bytes"] = err.stat().st_size if err.exists() else 0
        verdicts: list[str] = []
        for path in (log, err):
            if not path.exists():
                continue
            for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
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
        after = self._hash_settings()
        if after is not None:
            self.record["settings_sha256_after"] = after
            self.record["settings_changed"] = (
                self.record.get("settings_sha256_before") != after
            )

    def terminate(self, code: int = 137, reason: str = "injected process drop") -> None:
        if self._proc is None or self.poll() is not None:
            raise RuntimeError("the test process is not running")
        self.record["injected_termination"] = reason
        self._signal_group(signal.SIGKILL)
        self._save()
        _ = code

    def finish(self) -> dict[str, Any]:
        code = None
        while code is None:
            code = self.poll()
            if code is None:
                time.sleep(0.1)
        self.record["exit_code"] = code
        elapsed = 0.0 if self.start_time is None else time.monotonic() - self.start_time
        self.record["elapsed_seconds"] = round(elapsed, 3)
        self.record["ended_utc"] = utc_now()
        self.record["input_desktop_after"] = "n/a"
        self.record.setdefault("timed_out", False)
        self.close()
        self._collect_evidence()
        self._save()
        return self.record

    def close(self) -> None:
        if self._stdout is not None:
            self._stdout.close()
            self._stdout = None
        if self._stderr is not None:
            self._stderr.close()
            self._stderr = None
        if self._proc is not None:
            if self._proc.poll() is None:
                self._signal_group(signal.SIGKILL)
                try:
                    self._proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
                self.record.setdefault("exit_code", 125)
            self._proc = None
        self.record["job_closed"] = True
        self._save()


def run(
    argv: Sequence[str | os.PathLike[str]],
    cwd: Path | str,
    out: Path | str,
    timeout: float = 90,
    env: Mapping[str, str] | None = None,
    evidence_expected: Sequence[Path | str] | None = None,
    startup_checks: bool = True,
) -> dict[str, Any]:
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
