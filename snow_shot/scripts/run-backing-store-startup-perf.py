#!/usr/bin/env python3
"""Run the current Cocoa backing-store benchmark serially with balanced mode order."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_cache_for_binary(binary):
    """Find the nearest enclosing CMake build for a resolved binary path."""
    for directory in binary.parents:
        cache = directory / "CMakeCache.txt"
        if cache.is_file():
            return cache
    raise RuntimeError(f"cannot find CMakeCache.txt for selected binary: {binary}")


def distribution(values):
    values = sorted(values)
    return {
        "count": len(values),
        "median": statistics.median(values),
        "p95": values[math.ceil(0.95 * len(values)) - 1],
        "min": values[0],
        "max": values[-1],
    }


def add_memory_growth_metrics(record):
    """Calculate each capture's growth before combining capture distributions."""
    for kind in ["footprint", "resident"]:
        field = f"{kind}_bytes"
        before = record["memory_before"][field]
        delivery = record["memory_at_delivery"][field]
        first_frame = record["memory_first_frame"][field]
        record[f"capture_start_to_delivery_{kind}_growth_bytes"] = delivery - before
        record[f"delivery_to_first_frame_{kind}_growth_bytes"] = first_frame - delivery
        record[f"capture_start_to_first_frame_{kind}_growth_bytes"] = first_frame - before
        record[f"maximum_sampled_{kind}_growth_bytes"] = (
            record[f"maximum_sampled_{kind}_bytes"] - before
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qt-plugin-directory", type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--pairs", type=int, default=4)
    parser.add_argument("--native-capture", action="store_true")
    parser.add_argument("--native-width", type=int, default=3840)
    parser.add_argument("--native-height", type=int, default=2160)
    args = parser.parse_args()
    if args.rounds <= 0 or args.pairs <= 0:
        parser.error("--rounds and --pairs must be positive")
    if args.native_width <= 0 or args.native_height <= 0:
        parser.error("--native-width and --native-height must be positive")
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[2]
    cache = build_cache_for_binary(binary)
    cache_text = cache.read_text()
    if not re.search(r"^CMAKE_BUILD_TYPE:STRING=Release$", cache_text, re.M):
        raise RuntimeError("benchmark must use the Release performance preset")
    plugin_directory = args.qt_plugin_directory
    if plugin_directory is None:
        qt_core = re.search(r"^Qt6Core_DIR:PATH=(.+)$", cache_text, re.M)
        if not qt_core:
            raise RuntimeError("cannot resolve the configured Qt kit; specify --qt-plugin-directory")
        plugin_directory = Path(qt_core.group(1)).parents[2] / "plugins"
    plugin_directory = plugin_directory.resolve()
    cocoa = plugin_directory / "platforms/libqcocoa.dylib"
    environment = os.environ.copy()
    # Pin the installed configured kit; historical qt.conf files in test-bin
    # must not choose an experimental Cocoa plugin from an earlier benchmark.
    environment["QT_PLUGIN_PATH"] = str(plugin_directory)
    qt_args = ["-platform", "cocoa", "-platformpluginpath", str(cocoa.parent)]
    qualification_environment = environment | {"QT_DEBUG_PLUGINS": "1"}
    qualification = subprocess.run(
        [str(binary), *qt_args, "--help"], env=qualification_environment,
        text=True, capture_output=True, check=True,
    )
    qualification_log = qualification.stdout + qualification.stderr
    (output / "loader-qualification.log").write_text(qualification_log)
    loaded_plugins = re.findall(r'"([^"\n]*libqcocoa\.dylib)"\s+loaded library', qualification_log)
    if loaded_plugins != [str(cocoa)]:
        raise RuntimeError(f"unexpected loaded Cocoa plugin: {loaded_plugins}")
    changed = subprocess.check_output(["git", "diff", "--name-only", "HEAD"], cwd=repo, text=True)
    source_paths = [repo / path for path in changed.splitlines() if (repo / path).is_file()]
    source_paths += [repo / "snow_shot/tests/screenshot_backing_store_startup_performance_benchmark.cpp",
                     Path(__file__).resolve()]
    manifest = {
        "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
        "binary": str(binary), "binary_sha256": sha256(binary),
        "cocoa_plugin": str(cocoa), "cocoa_sha256": sha256(cocoa),
        "cmake_cache": str(cache), "cmake_cache_sha256": sha256(cache),
        "platform": platform.platform(),
        "source_sha256": {str(path.relative_to(repo)): sha256(path) for path in source_paths},
        "rounds": args.rounds, "pairs_per_process": args.pairs,
        "native_capture": args.native_capture,
        "orders": [["disabled", "allocation-only", "page-touch"],
                   ["page-touch", "disabled", "allocation-only"],
                   ["allocation-only", "page-touch", "disabled"]],
        "scope": "Same current-source binary and configured installed Cocoa plugin. Each mode/configuration "
                 "runs in a fresh process; each pair creates a cold widget then reuses that object after native "
                 "surface destruction. Acquisition is a controlled worker deadline with preallocated source "
                 "images; native capture GPU/permission and acquisition memory bandwidth are excluded. "
                 "All process-first samples retained. First frame means native Qt dispatch completion, "
                 "including backing-store flush, without a compositor presentation fence.",
        "runs": [],
    }
    if args.native_capture:
        manifest["scope"] = (
            "Same current-source binary and configured installed Cocoa plugin. Actual primary-display "
            "acquisition through the production ScreenCaptureKit FFI/lease wrapper, with a fresh session "
            "per sample, concurrent with GUI native preparation. No captured image is saved. "
            "Fresh process per mode/round; cold/recreated refers to the overlay QObject/native store, "
            "not the capture session. All process-first samples retained. First frame means native Qt "
            "dispatch completion including backing-store flush, without a compositor fence."
        )
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    groups = {}
    sizes = [(args.native_width, args.native_height)] if args.native_capture else [(1920, 1080), (3840, 2160)]
    delays = [0] if args.native_capture else [0, 20, 100]
    for round_index in range(args.rounds):
        order = manifest["orders"][round_index % 3]
        for width, height in sizes:
            for delay in delays:
                for mode in order:
                    name = f"round{round_index + 1}-{width}x{height}-delay{delay}-{mode}"
                    report_path = output / f"{name}.json"
                    command = [str(binary), *qt_args, "--mode", mode, "--width", str(width),
                               "--height", str(height), "--delay-ms", str(delay), "--pairs",
                               str(args.pairs), "--output", str(report_path)]
                    if args.native_capture:
                        command.append("--native-capture")
                    completed = subprocess.run(command, env=environment, text=True, capture_output=True)
                    (output / f"{name}.log").write_text(completed.stdout + completed.stderr)
                    manifest["runs"].append({"name": name, "command": command,
                                             "exit_code": completed.returncode})
                    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
                    if completed.returncode != 0:
                        raise RuntimeError(f"benchmark failed: {name}; inspect saved log/report")
                    report = json.loads(report_path.read_text())
                    for record in report["records"]:
                        add_memory_growth_metrics(record)
                        key = f"{width}x{height}/delay{delay}/{record['phase']}/{mode}"
                        record["round"] = round_index + 1
                        groups.setdefault(key, []).append(record)
                    print(f"completed {name}", flush=True)
    summary = {
        "scope": manifest["scope"],
        "sample_count": sum(len(records) for records in groups.values()),
        "correctness_failures": [], "groups": {}, "page_touch_vs_disabled": {},
        "limitations": ["Twelve samples per cell at the default settings give coarse p95; no confidence interval.",
                        "GUI CPU includes shared observer/memory sampling overhead. Native dispatch and canvas CPU overlap.",
                        "Sampled memory maxima may miss transient peaks; process peak RSS includes setup and all samples.",
                        "A source-ready deadline with preallocated opaque pixels does not reproduce capture bandwidth contention.",
                        "One physical display, actual DPR reported; 1080p and 4K denote source pixel dimensions.",
                        "Interleaved process rounds reduce but do not eliminate activation, allocator, thermal and OS scheduling noise."],
    }
    if args.native_capture:
        summary["limitations"][3] = (
            "Fresh ScreenCaptureKit session per sample includes real acquisition contention but "
            "does not represent a prewarmed production capture session. GUI CPU excludes worker CPU."
        )
    for key, records in groups.items():
        failures = [record for record in records if not record["correct"]]
        summary["correctness_failures"] += [{"case": key, "record": record} for record in failures]
        metrics = [name for name, value in records[0].items()
                   if isinstance(value, (int, float)) and not isinstance(value, bool)
                   and name not in ["iteration", "round", "width", "height", "delay_ms"]]
        summary["groups"][key] = {name: distribution([record[name] for record in records])
                                  for name in metrics}
    for key, values in summary["groups"].items():
        if not key.endswith("/page-touch"):
            continue
        baseline = summary["groups"][key.rsplit("/", 1)[0] + "/disabled"]
        comparisons = {}
        for name, value in values.items():
            comparisons[name] = {
                "disabled": baseline[name], "page_touch": value,
                "median_delta": value["median"] - baseline[name]["median"],
                "p95_delta": value["p95"] - baseline[name]["p95"],
            }
        summary["page_touch_vs_disabled"][key.rsplit("/", 1)[0]] = comparisons
    if sha256(binary) != manifest["binary_sha256"] or sha256(cocoa) != manifest["cocoa_sha256"]:
        raise RuntimeError("measured binary/plugin changed during the run")
    for path, expected_hash in manifest["source_sha256"].items():
        if sha256(repo / path) != expected_hash:
            raise RuntimeError(f"source changed during the run: {path}")
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"saved {summary['sample_count']} captures to {output / 'summary.json'}", flush=True)


if __name__ == "__main__":
    main()
