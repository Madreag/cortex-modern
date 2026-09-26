"""POSIX IsolatedRun. No firewall check on POSIX.

On the Mac an engine whose responsible process lives in the GUI login session (a launchd gui/<uid> job) raises
macOS's Local Network consent dialog once per new binary path, and a pending dialog blocks that binary's LAN
traffic. A runner that finds itself in that session therefore starts the engine through
`ssh -o BatchMode=yes localhost` (same argv, cwd and environment; never prompted), keeps the engine's streams and
exit code through the hop and kills the far process group itself on a timeout. CCCP_POSIX_HOP=ssh|off forces it.
Launches of desktop tools (osascript, screencapture, System Events) are refused, and so is a launch from a lane
script that calls them. `--self-test` proves the exit, stream, timeout and kill paths with and without the hop.
"""

from __future__ import annotations

import datetime
import hashlib
import json
import os
import re
import shlex
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

# Tools that act on the user's desktop session: a lane never runs them.
DESKTOP_TOOL = re.compile(r"\b(?:osascript|screencapture)\b|System Events")
DESKTOP_TOOL_NAMES = {"osascript", "screencapture"}
LANE_SCRIPT_SUFFIXES = {".sh", ".zsh", ".bash", ".command", ".py", ".pl", ".rb", ".js"}
# The variables that name the session a process runs in; the far side of the hop keeps its own.
SESSION_ENV = {"SSH_AUTH_SOCK", "SSH_CONNECTION", "SSH_CLIENT", "SSH_TTY", "XPC_SERVICE_NAME", "XPC_FLAGS",
               "__CFBundleIdentifier", "LaunchInstanceID", "SECURITYSESSIONID", "TERM_SESSION_ID"}
HOP_SSH = ["/usr/bin/ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=20", "localhost"]
_HOP_REACHABLE: bool | None = None
_LANE_SCRIPT_HITS: list[str] | None = None

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
        if Path(text).name in DESKTOP_TOOL_NAMES or "System Events" in text:
            raise ArgumentPolicyError(f"argv[{index}] names a desktop tool ({text!r}); lanes never drive the user's session.")
        out.append(text)
    return out


def _output(command: Sequence[str], timeout: float = 20) -> str | None:
    try:
        done = subprocess.run(list(command), capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout if done.returncode == 0 else None


def gui_session() -> bool:
    """True when this process belongs to the Mac's GUI login session rather than an ssh or background one."""
    if sys.platform != "darwin":
        return False
    manager = (_output(["/bin/launchctl", "managername"]) or "").strip()
    if manager and manager != "Background":
        return True
    return not os.environ.get("SSH_CONNECTION") and _output(["/bin/launchctl", "print", f"gui/{os.getuid()}"]) is not None


def hop_mode() -> str | None:
    """'ssh' when the engine must start through ssh localhost, None for a direct child."""
    forced = os.environ.get("CCCP_POSIX_HOP", "").strip().lower()
    if forced in ("ssh", "off"):
        return "ssh" if forced == "ssh" else None
    return "ssh" if gui_session() else None


def hop_reachable() -> bool:
    global _HOP_REACHABLE
    if _HOP_REACHABLE is None:
        try:
            done = subprocess.run([*HOP_SSH, "true"], stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
            _HOP_REACHABLE = done.returncode == 0
        except (OSError, subprocess.SubprocessError):
            _HOP_REACHABLE = False
    return _HOP_REACHABLE


def ancestor_commands(limit: int = 32) -> list[str]:
    commands: list[str] = []
    pid = os.getpid()
    for _ in range(limit):
        text = _output(["ps", "-o", "ppid=,args=", "-p", str(pid)])
        if not text or not text.strip():
            break
        parent, _, args = text.strip().partition(" ")
        commands.append(args.strip())
        if not parent.strip().isdigit() or int(parent) <= 1:
            break
        pid = int(parent)
    return commands


def lane_script_desktop_uses(commands: Sequence[str] | None = None) -> list[str]:
    """Lines of the calling lane's scripts (the ancestors' script arguments) that run a desktop tool."""
    hits: list[str] = []
    seen = {Path(__file__).resolve()}
    for command in ancestor_commands() if commands is None else commands:
        for token in command.split():
            path = Path(token)
            try:
                if not path.is_file() or path.resolve() in seen or path.stat().st_size > (4 << 20):
                    continue
                seen.add(path.resolve())
                data = path.read_bytes()
            except OSError:
                continue
            if b"\0" in data[:4096] or not (path.suffix in LANE_SCRIPT_SUFFIXES or data.startswith(b"#!")):
                continue
            for number, line in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
                if not line.lstrip().startswith("#") and DESKTOP_TOOL.search(line):
                    hits.append(f"{path}:{number}: {line.strip()[:160]}")
    return hits


def hop_far_side(state_path: str) -> int:
    """The ssh side of the hop: start the engine from the spec on stdin, record its pid and exit code."""
    spec = json.loads(sys.stdin.read())
    state = Path(state_path)
    proc = subprocess.Popen(spec["argv"], cwd=spec["cwd"], env=spec["env"], stdin=subprocess.DEVNULL, start_new_session=True)
    record: dict[str, Any] = {"engine_pid": proc.pid, "far_side_pid": os.getpid(), "started_utc": utc_now()}
    state.write_text(json.dumps(record), encoding="utf-8")

    def stop(signum: int, _frame: Any) -> None:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        raise SystemExit(128 + signum)

    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(sig, stop)
    while proc.poll() is None:
        # A runner that went away (its ssh gone, this side re-parented) takes its engine with it.
        if os.getppid() == 1:
            os.killpg(proc.pid, signal.SIGKILL)
        time.sleep(0.2)
    record.update(exit_code=proc.returncode, ended_utc=utc_now())
    state.write_text(json.dumps(record), encoding="utf-8")
    return proc.returncode if proc.returncode >= 0 else 128 - proc.returncode


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
    fixtures: Sequence[str] | None = None,
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
    fixture = repo_path / "tools/fixtures/preview_window_modcompat.lua"
    if fixture.is_file():
        dest = runtime / "tools/fixtures"
        dest.mkdir(parents=True, exist_ok=True)
        (dest / fixture.name).write_bytes(fixture.read_bytes())
    # The binary is not beside tools/ here, so a row's named fixtures resolve from the runtime.
    staged = [str(name) for name in (fixtures or [])]
    if staged:
        dest = runtime / "tools/fixtures"
        dest.mkdir(parents=True, exist_ok=True)
        for name in staged:
            (dest / Path(name).name).write_bytes((repo_path / "tools/fixtures" / name).read_bytes())
    exe = resolve_binary(repo_path, binary)
    manifest = {
        "executable": str(exe),
        "cwd": str(runtime),
        "data": str(data_path),
        "settings_sha256": hashlib.sha256(settings.encode()).hexdigest(),
        "settings_overrides": SETTINGS_OVERRIDES,
        "fixtures": staged,
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
    fixtures: Sequence[str] | None = None,
) -> IsolatedRun:
    repo_path = Path(repo).resolve()
    out_path = Path(out).resolve()
    out_path.mkdir(parents=True, exist_ok=False)
    runtime = prepare_runtime(repo_path, out_path, data=data, binary=binary, fixtures=fixtures)
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
            self.env.setdefault("CC_TEST_CRASH_DUMP", "crash.dmp")
            env_set["CC_TEST_CRASH_DUMP"] = self.env["CC_TEST_CRASH_DUMP"]
        self._proc: subprocess.Popen[bytes] | None = None
        self._stdout: Any = None
        self._stderr: Any = None
        self.start_time: float | None = None
        self.hop = hop_mode()
        self.hop_state = self.out / "hop.json"
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
            "hop": {"via": " ".join(HOP_SSH), "state": str(self.hop_state)} if self.hop else None,
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
        global _LANE_SCRIPT_HITS
        if _LANE_SCRIPT_HITS is None:
            _LANE_SCRIPT_HITS = lane_script_desktop_uses()
        self._check("lane_scripts_desktop_free", not _LANE_SCRIPT_HITS, "; ".join(_LANE_SCRIPT_HITS[:5]) or "no desktop tool")
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
            if self.hop and not hop_reachable():
                raise StartupCheckError(f"hop: `{' '.join(HOP_SSH)} true` failed; no engine is started in the GUI session")
            self._stdout = stdout_path.open("wb", buffering=0)
            self._stderr = stderr_path.open("wb", buffering=0)
            if self.hop:
                far = shlex.join([sys.executable, str(Path(__file__).resolve()), "--hop-exec", str(self.hop_state)])
                self._proc = subprocess.Popen(
                    [*HOP_SSH, far],
                    stdin=subprocess.PIPE,
                    stdout=self._stdout,
                    stderr=self._stderr,
                    start_new_session=True,
                )
                spec = {"argv": self.argv, "cwd": str(self.cwd), "env": {k: v for k, v in self.env.items() if k not in SESSION_ENV}}
                try:
                    self._proc.stdin.write(json.dumps(spec).encode("utf-8"))
                    self._proc.stdin.close()
                except BrokenPipeError:
                    pass
            else:
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

    def _hop_record(self, wait_s: float = 0.0) -> dict[str, Any]:
        deadline = time.monotonic() + wait_s
        while True:
            try:
                return json.loads(self.hop_state.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                if time.monotonic() >= deadline:
                    return {}
                time.sleep(0.05)

    def _signal_group(self, sig: int) -> None:
        if self._proc is None or self._proc.pid is None:
            return
        if self.hop:
            # The engine leads its own group on the far side of the hop; the same uid on the same box signals it.
            engine = self._hop_record(wait_s=2.0).get("engine_pid")
            if engine:
                try:
                    os.killpg(engine, sig)
                except ProcessLookupError:
                    pass
                if sig != signal.SIGKILL:
                    return
                # The far side records the exit and closes the hop; only a hop that outlives that is killed too.
                try:
                    self._proc.wait(timeout=2)
                    return
                except subprocess.TimeoutExpired:
                    pass
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
        if code is not None and self.hop:
            far = self._hop_record(wait_s=1.0)
            self.record["hop"].update(ssh_exit=code, engine_pid=far.get("engine_pid"))
            if "exit_code" in far:
                return far["exit_code"]
            self.record["hop"]["failure"] = "the far side recorded no engine exit"
            return code
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


def _pid_alive(path: Path) -> bool:
    try:
        os.kill(int(path.read_text().strip()), 0)
        return True
    except (OSError, ValueError):
        return False


def self_test(modes: Sequence[str]) -> int:
    """The runner's own rows: refusals, then exit code, streams, signal, timeout and kill with and without the hop."""
    import tempfile

    failures: list[str] = []

    def expect(ok: bool, what: str) -> None:
        print(f"[posix-runner-selftest] {'PASS' if ok else 'FAIL'} {what}", flush=True)
        if not ok:
            failures.append(what)

    for argv in (["/usr/bin/osascript", "-e", "beep"], ["screencapture", "-x", "shot.png"], ["/bin/sh", "-c", 'tell application "System Events"']):
        try:
            validate_argv(argv)
            refused = False
        except ArgumentPolicyError:
            refused = True
        expect(refused, f"refuses a launch of {argv[0]}")
    with tempfile.TemporaryDirectory() as scratch:
        calling = Path(scratch) / "lane.zsh"
        calling.write_text("#!/bin/zsh\n# a lane step\n" + "screen" + "capture -x shot.png\n")
        commented = Path(scratch) / "notes.zsh"
        commented.write_text("#!/bin/zsh\n# never call osa" + "script here\necho ok\n")
        expect(bool(lane_script_desktop_uses([f"/bin/zsh {calling}"])), "refuses a lane script that runs a desktop tool")
        expect(not lane_script_desktop_uses([f"/bin/zsh {commented}"]), "passes a lane script that only mentions one in a comment")
    saved = os.environ.get("CCCP_POSIX_HOP")
    try:
        for mode in modes:
            os.environ["CCCP_POSIX_HOP"] = mode
            with tempfile.TemporaryDirectory() as scratch:
                root = Path(scratch)
                record = run(["/bin/sh", "-c", "echo out; echo err >&2; exit 7"], root, root / "exit", 30, startup_checks=False)
                streams = ((root / "exit/stdout.log").read_text(), (root / "exit/stderr.log").read_text())
                expect(record["exit_code"] == 7 and streams == ("out\n", "err\n") and bool(record.get("hop")) == (mode == "ssh"),
                       f"{mode}: exit code 7 and both streams kept (got {record['exit_code']}, {streams!r})")
                record = run(["/bin/sh", "-c", "kill -TERM $$"], root, root / "signal", 30, startup_checks=False)
                expect(record["exit_code"] == -signal.SIGTERM, f"{mode}: a signal death reads as -15 (got {record['exit_code']})")
                group = ["/bin/sh", "-c", "sleep 300 & echo $! > child.pid; echo $$ > parent.pid; wait"]
                started = time.monotonic()
                record = run(group, root, root / "timeout", 3, startup_checks=False)
                time.sleep(0.5)
                alive = [name for name in ("parent.pid", "child.pid") if _pid_alive(root / name)]
                expect(record.get("timed_out") is True and record["exit_code"] == 124 and not alive and time.monotonic() - started < 20,
                       f"{mode}: a timeout ends the whole group (exit {record['exit_code']}, alive {alive})")
                for name in ("parent.pid", "child.pid"):
                    (root / name).unlink(missing_ok=True)
                drop = IsolatedRun(group, root, root / "drop", 60, startup_checks=False)
                try:
                    drop.start()
                    deadline = time.monotonic() + 15
                    while not (root / "child.pid").is_file() and time.monotonic() < deadline:
                        time.sleep(0.05)
                    drop.terminate()
                    record = drop.finish()
                finally:
                    drop.close()
                time.sleep(0.5)
                alive = [name for name in ("parent.pid", "child.pid") if _pid_alive(root / name)]
                expect(record["exit_code"] == -signal.SIGKILL and not alive,
                       f"{mode}: an injected drop kills the group and reads as -9 (exit {record['exit_code']}, alive {alive})")
    finally:
        if saved is None:
            os.environ.pop("CCCP_POSIX_HOP", None)
        else:
            os.environ["CCCP_POSIX_HOP"] = saved
    print(f"[posix-runner-selftest] {'PASS' if not failures else 'FAIL'} modes={','.join(modes)} gui_session={gui_session()}", flush=True)
    return 0 if not failures else 1


if __name__ == "__main__":
    import argparse

    if sys.argv[1:2] == ["--hop-exec"]:
        sys.exit(hop_far_side(sys.argv[2]))
    if sys.argv[1:2] == ["--self-test"]:
        sys.exit(self_test(sys.argv[2:] or ["off", "ssh"]))
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
