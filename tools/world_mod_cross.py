"""World and mod specializations of the cross driver's reserved runs.

The integration patch calls these hooks only for an explicitly named acceptance
row. Ordinary match plans and their reports keep the cross driver's own path.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import time
import urllib.error
import urllib.request

from acceptance_mod import manifest as mod_manifest, equal_manifests
from acceptance_runtime import write_json

ROWS = ("mod-match", "mod-refusal", "world-join", "world-soak")
DRIVER_FILES = ("cross_peers.py", "cross_report.py", "e2e_video.py", "feel/report.py", "feel/records.py", "world_mod_cross.py",
                "world_soak.py", "acceptance_rows.py", "acceptance_evidence.py", "acceptance_cross_report.py", "acceptance_fixed_gates.py", "acceptance_mod.py")


def named_row(arguments):
    for index, argument in enumerate(arguments):
        if argument == '--acceptance-row' and index+1 < len(arguments):
            return arguments[index+1] in ROWS
        if argument.startswith('--acceptance-row='):
            return argument.partition('=')[2] in ROWS
    return False


def is_row(document):
    return document.get('acceptance_row') in ROWS


def flag(arguments, name, value=None):
    arguments = list(arguments)
    if name in arguments:
        index = arguments.index(name)
        arguments.pop(index)
        if index < len(arguments) and not arguments[index].startswith("-"):
            arguments.pop(index)
    if value is not False:
        arguments += [name] + ([] if value is None else [str(value)])
    return arguments


def builtin_directory(repo):
    source = (Path(repo)/"Source/Managers/SettingsMan.h").read_text(encoding="utf-8")
    match = re.search(r'c_DefaultSessionDirectoryUrl\s*=\s*"([^"\r\n]+)"', source)
    if not match:
        raise ValueError("built-in public directory constant is missing")
    return match[1]


def configure_plan(plan, row, mod_receipts=None):
    if row not in ROWS:
        raise ValueError("unknown world/mod acceptance row")
    plan = deepcopy(plan)
    from acceptance_mod import sha256
    for module in DRIVER_FILES:
        plan.setdefault("driver_sources", {})[module] = sha256(Path(__file__).parent/module)
    names = {spec["peer"] for spec in plan["specs"]}
    if names != {"erol", "edith", "mac", "linux"} or plan["host"] != "erol":
        raise ValueError("acceptance requires the four named boxes with the PC hosting")
    if len({spec["box"] for spec in plan["specs"]}) != 4:
        raise ValueError("acceptance requires four distinct machines")
    world = row.startswith("world-")
    if row == "world-soak":
        plan["specs"] = [s for s in plan["specs"] if s["peer"] in ("erol", "edith")]
        plan["instances"] = [s for s in plan["instances"] if s["name"] in ("erol", "edith")]
        used = {s["box"] for s in plan["specs"]}
        plan["boxes"] = [b for b in plan["boxes"] if b["name"] in used]
        late = next(s for s in plan['specs'] if s['peer'] == 'edith')
        def initial_copy(value):
            if isinstance(value, str): return value.replace('/edith/', '/edith-first/')
            if isinstance(value, list): return [initial_copy(item) for item in value]
            if isinstance(value, dict): return {key: initial_copy(item) for key, item in value.items()}
            return value
        initial = initial_copy(late)
        initial['peer'] = 'edith-first'
        initial['env']['CC_TEST_CROSS_INSTANCE'] = 'edith-first'
        initial['flags'] = flag(initial['flags'], '-net-player-name', 'edith-first')
        if 'port_block' in initial:
            initial['port_block'] = [port+5 for port in initial['port_block']]
            initial['flags'] = flag(initial['flags'], '-net-port', initial['port_block'][0])
        plan['specs'].insert(1, initial)
        template = next(p for p in plan['instances'] if p['name'] == 'edith')
        plan['instances'].insert(1, {**deepcopy(template), 'name':'edith-first', **({'port_block':initial['port_block']} if 'port_block' in initial else {})})
        for box in plan['boxes']:
            if box['name'] == late['box']:
                box['peers_per_box'] = 2
                if 'ports' in box and initial['port_block'][-1] > box['ports'][-1]:
                    raise ValueError('EDITH needs two disjoint instance port blocks')
        from world_soak import configuration
        plan["soak"] = configuration(max(3660, (plan["ticks"]-1)//60))
        plan["ticks"] = plan["soak"]["ticks"]
    elif row == "world-join":
        plan["ticks"] = max(3601, plan["ticks"])
    else:
        plan["ticks"] = 1201
        if mod_receipts is None:
            raise ValueError("four complete module manifests are required before a mod run")
        digest = equal_manifests(mod_receipts)
        plan["module_tree_sha256"] = digest
    plan.update(acceptance_row=row, fullstate_every=60, faults=[], capture_barriers=[], preserve_evidence=True)
    if row == "world-join":
        plan["late_join"] = dict(peer="edith", host_tick=1200)
    elif row == "world-soak":
        plan["late_join"] = dict(peer="edith", host_elapsed_s=3000)
    elif row == "mod-refusal":
        plan["late_join"] = dict(peer="linux", host_tick=600)
    else:
        plan["late_join"] = None
    plan["directory_mode"] = "public-default"
    plan["deadlines"]["reservation_s"] = 3600
    plan["deadlines"]["launch_s"] = max(plan["deadlines"]["launch_s"], plan["ticks"]//60+600)
    peers = 3 if row == "mod-refusal" else len(plan["specs"])
    for spec in plan["specs"]:
        spec.update(acceptance_row=row, preserve_evidence=True, ticks=plan["ticks"], faults=[], recoveries=[], forced_ends=[], barriers=[],
                    timeout=plan["deadlines"]["launch_s"], directory_mode="public-default",
                    scene=plan["scene"] if world else "Void Wanderers", scene_module="Base.rte" if world else "VoidWanderers.rte",
                    session_wait_s=3600 if row == "world-soak" else 240)
        flags = spec["flags"]
        for name, value in (("-max-ticks", plan["ticks"]), ("-net-match-ticks", plan["ticks"]-1),
                            ("-net-match-peers", peers), ("-net-match-humans", peers-1 if world else peers), ("-net-match-cpu-slots", 0),
                            ("-net-match-service-preset", "Persistent World" if world else "Void Wanderers"),
                            ("-net-match-service-module", "Base.rte" if world else "VoidWanderers.rte"),
                            ("-net-match-service-scene", spec["scene"]), ("-net-match-service-scene-module", spec["scene_module"]),
                            ("-net-fullstate-hash-every", 60), ("-net-fullstate-dump", False),
                            ("-net-cross-rematches", False), ("-net-cross-host-options", False),
                            ("-net-autosave-seconds", 60 if row == "world-soak" else 0),
                            ("-memory-census-ticks", 3600 if row == "world-soak" else 60)):
            flags = flag(flags, name, value)
        if world and spec["role"] == "host":
            flags = flag(flag(flags, "-net-persistent-world"), "-net-world-fresh")
        if not world:
            flags = flag(flags, "-module", "VoidWanderers.rte")
            spec["module_tree_sha256"] = plan["module_tree_sha256"]
            box = next(value for value in plan["boxes"] if value["name"] == spec["box"])
            spec["module_source"] = box.get("module_source", spec["repo"]+"/Data/VoidWanderers.rte")
        spec["flags"] = flags
        if plan['late_join'] and spec['peer'] == plan['late_join']['peer']:
            spec.update(defer_until_session=True, session_leaf='session-late.json')
        else:
            spec['session_leaf'] = 'session.json'
        spec["env"].pop("CC_TEST_CROSS_BOT", None)
        spec["env"].pop("CC_TEST_CROSS_CAPTURE_BARRIER", None)
        spec["env"].update(CCCP_HEADLESS="1", CC_RUNNER_IGNORE_FULLSCREEN="1")
        if row == "mod-refusal" and spec["peer"] == "linux":
            spec["module_refusal"] = True
        if row == "world-soak":
            spec["soak"] = plan["soak"]
    return plan


def preflight_mod(box, result):
    path = Path(box.get("module_source", str(Path(box["tree"])/"Data/VoidWanderers.rte")))
    module = mod_manifest(path)
    staged = {"VoidWanderers.rte/"+entry["path"]:entry["sha256"] for entry in module["files"]}
    content = result.setdefault("content", {})
    installed = {name:digest for name,digest in content.items() if name.startswith("VoidWanderers.rte/")}
    if installed and installed != staged:
        raise ValueError("base Data mod differs from the selected unchanged module")
    # The private runtime adds this exact copy to Data; every one of its files remains in the full content comparison.
    content.update(staged)
    result.setdefault("modules", {})["VoidWanderers.rte/Index.ini"] = staged["VoidWanderers.rte/Index.ini"]
    result["acceptance_module"] = module
    result["acceptance_module_source"] = str(path)


def preflight_driver(box, result):
    from acceptance_mod import sha256
    result['acceptance_driver_sources'] = {name: sha256(Path(box['tree'])/'tools'/name) for name in DRIVER_FILES}


def check_driver_preflights(plan, preflights):
    if not is_row(plan):
        return
    expected = {name: plan['driver_sources'][name] for name in DRIVER_FILES}
    if any(value.get('acceptance_driver_sources') != expected for value in preflights.values()):
        raise ValueError('acceptance driver source hashes differ across boxes; no match launched')


def check_mod_preflights(plan, preflights):
    if plan.get("acceptance_row") not in ("mod-match", "mod-refusal"):
        return
    values = {}
    for spec in plan["specs"]:
        canonical = "pc" if spec["peer"] == "erol" else spec["peer"]
        value = preflights.get(spec["box"], {}).get("acceptance_module")
        if value is None:
            raise ValueError("native preflight omitted the installed mod tree")
        values[canonical] = value
    if equal_manifests(values) != plan["module_tree_sha256"]:
        raise ValueError("module changed since its four-box install receipts")


def public_directory_available(repo):
    url = builtin_directory(repo)
    if "://" not in url:
        url = "https://"+url
    request = urllib.request.Request(url.rstrip("/")+"/v1/sessions", headers={"User-Agent": "CortexCommand-Acceptance"})
    try:
        with urllib.request.urlopen(request, timeout=10) as reply:
            return reply.status < 500
    except urllib.error.HTTPError as error:
        return error.code < 500
    except (urllib.error.URLError, TimeoutError):
        return False


def directory_settings(spec, settings):
    if spec.get("directory_mode") == "public-default":
        settings.update(SessionDirectoryUrl=builtin_directory(spec["repo"]), SessionDirectoryCertSha256="",
                        NetworkIceEnable="1", NetworkConnectionMode="DirectOnly", NetworkHostRelayMode="Off",
                        NetworkPortMapEnable="0")
    return settings


def published_session(plan):
    host = next(s for s in plan["specs"] if s["peer"] == plan["host"])
    path = Path(host["own"])/"engine/stdout.log"
    text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
    values = re.findall(r"(?m)^\[net-directory\] registered session_id=(\S+) heartbeat_s=\d+", text)
    return values[-1] if values else None


def latest_tick(path):
    path = Path(path)
    if not path.is_file():
        return None
    with path.open("rb") as stream:
        stream.seek(max(0, path.stat().st_size-65536))
        lines = stream.read().splitlines()
    for line in reversed(lines):
        try: row = json.loads(line)
        except (UnicodeError, ValueError): continue
        if row.get("phase") == "live" and type(row.get("tick")) is int:
            return row["tick"]
    return None


def late_join_due(plan, clock, now=None):
    now = time.monotonic() if now is None else now
    late = plan.get("late_join")
    if not late:
        return False
    host = next(s for s in plan["specs"] if s["peer"] == plan["host"])
    tick = latest_tick(Path(host["own"])/"live.jsonl")
    if tick is None:
        return False
    if "host_started" not in clock:
        clock["host_started"] = now
    if "host_tick" in late:
        return tick >= late["host_tick"]
    return now-clock["host_started"] >= late["host_elapsed_s"]


def prepare_mod_runtime(spec):
    """Create a fresh Data overlay; the installed original never sits behind the mutated path."""
    from acceptance_mod import pack, install
    from run_sim_test import RUNTIME_SETTINGS, seed_settings
    from types import SimpleNamespace
    own = Path(spec["own"])
    repo = Path(spec["repo"])
    runtime = own/"private-runtime"
    runtime.mkdir()
    for name in ("Data", "Mods", "ScreenShots", "Userdata", "Temp"):
        (runtime/name).mkdir()
    for source in sorted((repo/"Data").iterdir()):
        if source.name == "VoidWanderers.rte":
            continue
        target = runtime/"Data"/source.name
        if source.is_dir():
            if os.name == "nt":
                quote = lambda value: "'"+str(value).replace("'", "''")+"'"
                subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                                "New-Item -ItemType Junction -Path "+quote(target)+" -Target "+quote(source)+" | Out-Null"],
                               check=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                target.symlink_to(source.resolve(), target_is_directory=True)
        elif source.is_file() and not source.is_symlink():
            shutil.copyfile(source, target)
        else:
            raise ValueError("Data overlay contains an unsupported source entry")
    (runtime/"Userdata/Settings.ini").write_text("SettingsMan\n", encoding="utf-8")
    seed_settings(SimpleNamespace(cwd=runtime), RUNTIME_SETTINGS)
    archive, receipt = own/"module.tar", own/"module.json"
    expected = pack(Path(spec.get("module_source", str(repo/"Data/VoidWanderers.rte"))), archive, receipt)
    if expected["tree_sha256"] != spec["module_tree_sha256"]:
        raise ValueError("mod changed after preflight")
    install(archive, receipt, runtime/"Data/VoidWanderers.rte")
    write_json(own/"module-runtime.json", dict(runtime=str(runtime), module=str(runtime/"Data/VoidWanderers.rte"),
                                              tree_sha256=expected["tree_sha256"], settings_overrides=RUNTIME_SETTINGS))
    return runtime


def stage_activity(run, spec):
    from feel_measure import private_settings
    private_settings(run, spec.get("render_cap", 60))
    (Path(spec["own"])/"engine/feel").mkdir(exist_ok=True)
    if spec["acceptance_row"].startswith("mod-"):
        from acceptance_mod import alter_one_byte
        own = Path(spec["own"])
        module = Path(run.cwd)/"Data/VoidWanderers.rte"
        data_info = (Path(run.cwd)/"Data").lstat()
        if (Path(run.cwd).resolve() != (own/"private-runtime").resolve() or (Path(run.cwd)/"Data").is_symlink()
                or getattr(data_info, "st_file_attributes", 0) & 0x400
                or module.resolve().parent != (Path(run.cwd)/"Data").resolve()
                or mod_manifest(module)["tree_sha256"] != spec["module_tree_sha256"]):
            raise ValueError("mod run did not use its verified private Data copy")
        if spec.get("module_refusal"):
            selected = "Index.ini"
            raw = (module/selected).read_bytes()
            if not raw or raw[-1:] not in (b"\n", b"\r", b"\t"):
                raise ValueError("module Index.ini has no trailing whitespace for the one-byte refusal")
            mutation = alter_one_byte(module, selected, Path(spec["root"]).parent, own/"mutation.json", len(raw)-1, 32)
            try:
                stage_refusal_menu(run, spec, own, mutation)
            except BaseException:
                # This run has not entered the payload's process table yet.
                restore_activity(spec)
                raise


def stage_refusal_menu(run, spec, own, mutation):
    write_json(own/"mutation-summary.json", mutation)
    session = spec['flags'][spec['flags'].index('-net-join-session')+1]
    if not re.fullmatch(r'[A-Za-z0-9_-]+', session):
        raise ValueError('published session id is not safe for the menu script')
    menu = own/'refusal.menu.txt'
    menu.write_text('wait_ms 1980\nactivate ButtonMainToMultiplayer\nwait_ms 495\n'
                    'activate ButtonMultiplayerJoinGame\nwait_ms 495\n'
                    f'settext TextJoinAddress session:{session}\nsettext TextJoinPort {spec["port_block"][0]}\n'
                    'activate ButtonMultiplayerConnect\nwait_state Failed\nwait_ms 495\n'
                    'assert_substate Landing\nassert_enabled ButtonMultiplayerJoinGame 1\n'
                    'assert_visible LabelMultiplayerLandingStatus 1\nassert_text_fits LabelMultiplayerLandingStatus\n'
                    'assert_inside_screen LabelMultiplayerLandingStatus\n'
                    'dump_host_options\nwait_ms 990\nexit\n', encoding='utf-8')
    for name in ('-net-match-service-e2e', '-net-join-session'):
        run.argv[:] = flag(run.argv, name, False)
    run.argv[:] = flag(run.argv, '-menu-script', menu)
    spec['env'].pop('CC_TEST_NET_UI_SCRIPT', None)
    # The menu refusal has no game phase in which the ordinary gameplay probe could activate.
    run.env.pop('CC_TEST_NET_UI_SCRIPT', None)
    run.record.get('env_set', {}).pop('CC_TEST_NET_UI_SCRIPT', None)
    from e2e_video import SCREEN_WATCHES
    watches = own/'screen-watches.txt'
    watches.write_text(SCREEN_WATCHES, encoding='utf-8')
    run.env['CCCP_TEST_SCREEN_WATCHES'] = str(watches)
    run.record.setdefault('env_set', {})['CCCP_TEST_SCREEN_WATCHES'] = str(watches)


def restore_activity(spec):
    if spec.get("module_refusal"):
        from acceptance_mod import restore_one_byte
        path = Path(spec["own"])/"mutation.json"
        if path.is_file():
            restore_one_byte(path)


def retain_native_screens(spec, run):
    """Keep native label dumps outside the runtime excluded from remote evidence transfer."""
    if not is_row(spec):
        return
    own, index = Path(spec["own"]), []
    destination = own/"native-screens"
    destination.mkdir(exist_ok=True)
    from acceptance_mod import sha256
    for source in sorted((Path(run.cwd)/"ScreenShots").glob("*.json")):
        info = source.lstat()
        if not stat.S_ISREG(info.st_mode) or source.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("native screen dump is not a regular file")
        target = destination/source.name
        digest = sha256(source)
        if target.exists():
            if sha256(target) != digest:
                raise ValueError("retained screen dump changed")
        else:
            with source.open("rb") as original, target.open("xb") as output:
                shutil.copyfileobj(original, output)
        if sha256(target) != digest:
            raise ValueError("retained screen dump differs from its native source")
        index.append(dict(name=source.name, bytes=info.st_size, sha256=digest))
    write_json(own/"native-screens.json", index)


def observe_soak(spec, run, now):
    if spec.get("acceptance_row") != "world-soak":
        return
    tick = latest_tick(Path(spec["own"])/"live.jsonl")
    if tick is None:
        return
    spec.setdefault("_soak_clock", now)
    elapsed = now-spec["_soak_clock"]
    seen = spec.setdefault("_journal_minutes", [])
    if spec["role"] == "host":
        from world_soak import journal_receipt
        receipts = spec.setdefault("_journal_receipts", [])
        for minute in (10, 30, 50, 60):
            if minute in seen or elapsed < minute*60:
                continue
            receipts.append(journal_receipt(run.cwd, minute, elapsed))
            seen.append(minute)
            write_json(Path(spec["own"])/"journal-sizes.json", receipts)
    if elapsed >= spec.get("_soak_next_sample", 0):
        write_json(Path(spec["own"])/"soak-elapsed.json", dict(elapsed_s=elapsed, last_tick=tick))
        spec["_soak_next_sample"] = elapsed+60


def expected_refusal(spec, record):
    if not spec.get("module_refusal") or record.get("timed_out"):
        return False
    log = (Path(spec["own"])/"engine/stdout.log").read_text(encoding="utf-8", errors="replace")
    return record.get("exit_code") in (0, 1) and "ModuleManifestMismatch" in log


def fetch_preserved(box, root, local):
    import subprocess
    import tarfile
    from acceptance_box_mods import shell_command
    name = "edith" if box["kind"] == "windows-task" else "mac" if box["ssh"] == "Erol-Mac" else "linux"
    target = {**box, "name": name}
    prefix = ["env", "COPYFILE_DISABLE=1", "tar"] if name == "mac" else ["tar.exe" if name == "edith" else "tar"]
    exclusions = ["runtime", "private-runtime", "*.ticket", "*.key", "*.pem", "evidence.tar"]
    args = [*prefix, "-cf", "-", "-C", root, *("--exclude="+p for p in exclusions), "."]
    local = Path(local)
    local.mkdir(parents=True, exist_ok=True)
    archive = local/"evidence-preserved.tar"
    with archive.open("xb") as sink:
        subprocess.run(["ssh", "-o", "BatchMode=yes", box["ssh"], shell_command(target, args)],
                       stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.PIPE, check=True, timeout=1800,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    count = 0
    with tarfile.open(archive) as stream:
        for member in stream.getmembers():
            path = local/member.name
            if member.issym() or member.islnk() or not path.resolve().is_relative_to(local.resolve()):
                raise ValueError("remote evidence contains an unsafe path")
            if not member.isfile():
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            data = stream.extractfile(member)
            # Coordinator preflight/payload copies already exist; fetched copies remain separate.
            if path.exists():
                path = path.with_name(path.name+".remote")
            with path.open("xb") as output:
                for chunk in iter(lambda: data.read(1024**2), b""):
                    output.write(chunk)
            count += 1
    count_code = ("import os,sys; from pathlib import Path; n=0\n"
                  "for root,dirs,files in os.walk(sys.argv[1],followlinks=False):\n"
                  " dirs[:]=[d for d in dirs if d not in ('runtime','private-runtime') and not Path(root,d).is_symlink()]\n"
                  " n+=sum(f!='evidence.tar' and not f.endswith(('.ticket','.key','.pem')) for f in files)\n"
                  "print(n)")
    done = subprocess.run(["ssh", "-o", "BatchMode=yes", box["ssh"], shell_command(target, [box["python"], "-c", count_code, root])],
                          capture_output=True, text=True, check=True, timeout=120,
                          creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if int(done.stdout.strip()) != count:
        raise ValueError("remote evidence count differs after tar fetch")
    write_json(local/"fetch-count.json", dict(remote_files=count, local_files=count, removed_files=0))


def phase_b_ready(path):
    return re.search(r"(?m)^GO-PHASE-B(?:\s.*)?$", Path(path).read_text(encoding="utf-8-sig")) is not None


def main(argv=None):
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--acceptance-row", choices=ROWS, required=True)
    parser.add_argument("--mod-receipts", type=Path)
    args, rest = parser.parse_known_args(argv)
    import cross_peers
    if '--roster' not in rest:
        rest += ['--roster', 'four-way']
    options = cross_peers.parse_args(rest)
    plan = cross_peers.make_plan(options)
    manifests = json.loads(args.mod_receipts.read_text(encoding="utf-8-sig")) if args.mod_receipts else None
    plan = configure_plan(plan, args.acceptance_row, manifests)
    if options.dry_run:
        print(json.dumps(plan, indent=2))
        print('DRY RUN: public directory first; fallback only after an outage; no engines or listeners started')
        return 0
    if not hasattr(cross_peers, "acceptance_cross"):
        raise RuntimeError("Phase B cross-driver integration has not landed; no engine launched")
    return cross_peers.run_plan(plan, options.out)


if __name__ == "__main__":
    raise SystemExit(main())
