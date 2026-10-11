#!/usr/bin/env python3
"""Build qualification regressions for the Cocoa startup benchmark runner."""

from contextlib import redirect_stdout
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).with_name("run-backing-store-startup-perf.py")
SPEC = importlib.util.spec_from_file_location("backing_store_startup_runner", SCRIPT)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class BuildQualificationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name).resolve()
        self.script = self.repo / "snow_shot/scripts/run-backing-store-startup-perf.py"
        self.script.parent.mkdir(parents=True)
        self.script.write_bytes(SCRIPT.read_bytes())
        source = self.repo / "snow_shot/tests/screenshot_backing_store_startup_performance_benchmark.cpp"
        source.parent.mkdir(parents=True)
        source.write_text("// Isolated benchmark fixture.\n", encoding="utf-8")
        self.output = self.repo / "output"
        self.expected_binary = None
        self.expected_plugins = None

    def plugin_kit(self, name):
        kit = self.repo / "qt" / name
        core = kit / "lib/cmake/Qt6Core"
        core.mkdir(parents=True)
        plugins = kit / "plugins"
        cocoa = plugins / "platforms/libqcocoa.dylib"
        cocoa.parent.mkdir(parents=True)
        cocoa.write_bytes(f"Cocoa plugin for {name}".encode())
        return core, plugins

    def build(self, name, configuration="Release", cache=True):
        root = self.repo / "build" / name
        binary = root / "snow_shot/test-bin/snow-shot-backing-store-startup-benchmark"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(f"Benchmark for {name}".encode())
        core, plugins = self.plugin_kit(name)
        cache_path = root / "CMakeCache.txt"
        if cache:
            cache_path.write_text(
                f"CMAKE_BUILD_TYPE:STRING={configuration}\n"
                f"Qt6Core_DIR:PATH={core}\n"
                "SNOW_APPS_BUILD_BENCHMARKS:BOOL=ON\n",
                encoding="utf-8",
            )
        return binary, cache_path, plugins

    def git_output(self, command, *, cwd, text):
        self.assertEqual(Path(cwd), self.repo)
        self.assertTrue(text)
        if command == ["git", "diff", "--name-only", "HEAD"]:
            return ""
        if command == ["git", "rev-parse", "HEAD"]:
            return "0123456789abcdef0123456789abcdef01234567\n"
        self.fail(f"Unexpected external command: {command}")

    def benchmark_process(self, command, *, env, text, capture_output, check=False):
        self.assertEqual(Path(command[0]), self.expected_binary)
        self.assertEqual(env["QT_PLUGIN_PATH"], str(self.expected_plugins))
        self.assertEqual(command[command.index("-platform") + 1], "cocoa")
        self.assertEqual(
            command[command.index("-platformpluginpath") + 1],
            str(self.expected_plugins / "platforms"),
        )
        self.assertTrue(text)
        self.assertTrue(capture_output)
        if "--help" in command:
            self.assertTrue(check)
            self.assertEqual(env["QT_DEBUG_PLUGINS"], "1")
            cocoa = self.expected_plugins / "platforms/libqcocoa.dylib"
            return subprocess.CompletedProcess(command, 0, f'"{cocoa}" loaded library\n', "")
        report_path = Path(command[command.index("--output") + 1])
        records = [
            {
                "iteration": 0,
                "phase": phase,
                "correct": True,
                "first_native_dispatch_gui_cpu_ms": 1.0,
                "memory_before": {"footprint_bytes": 100, "resident_bytes": 200},
                "memory_at_delivery": {"footprint_bytes": 110, "resident_bytes": 210},
                "memory_first_frame": {"footprint_bytes": 120, "resident_bytes": 220},
                "maximum_sampled_footprint_bytes": 130,
                "maximum_sampled_resident_bytes": 230,
            }
            for phase in ("cold", "recreated")
        ]
        report_path.write_text(json.dumps({"records": records}), encoding="utf-8")
        return subprocess.CompletedProcess(command, 0, "", "")

    def invoke(self, binary, plugins, *arguments):
        self.expected_binary = binary.resolve()
        self.expected_plugins = plugins.resolve()
        argv = [
            str(self.script), "--binary", str(binary), "--output", str(self.output),
            "--rounds", "1", "--pairs", "1", *arguments,
        ]
        with patch.object(RUNNER, "__file__", str(self.script)), \
                patch.object(sys, "argv", argv), \
                patch.object(RUNNER.subprocess, "run", side_effect=self.benchmark_process) as run, \
                patch.object(RUNNER.subprocess, "check_output", side_effect=self.git_output), \
                patch.object(RUNNER.platform, "platform", return_value="isolated-test-platform"), \
                redirect_stdout(io.StringIO()):
            self.process_run = run
            RUNNER.main()
        manifest = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        summary = json.loads((self.output / "summary.json").read_text(encoding="utf-8"))
        self.assertEqual(summary["sample_count"], 36)
        self.assertEqual(summary["correctness_failures"], [])
        return manifest

    def test_x64_performance_runs_without_an_arm64_build(self):
        binary, cache, plugins = self.build("snow-shot-macos-x64-performance")
        manifest = self.invoke(binary, plugins)
        self.assertEqual(manifest["binary"], str(binary))
        self.assertEqual(manifest["cmake_cache"], str(cache))
        self.assertEqual(manifest["cocoa_plugin"], str(plugins / "platforms/libqcocoa.dylib"))

    def test_selected_build_owns_plugin_and_cache_manifest_when_builds_coexist(self):
        _, sibling_cache, _ = self.build("snow-shot-macos-arm64-performance")
        binary, selected_cache, plugins = self.build("snow-shot-macos-x64-performance")
        manifest = self.invoke(binary, plugins)
        expected_hash = hashlib.sha256(selected_cache.read_bytes()).hexdigest()
        sibling_hash = hashlib.sha256(sibling_cache.read_bytes()).hexdigest()
        self.assertNotEqual(expected_hash, sibling_hash)
        self.assertEqual(manifest["cmake_cache"], str(selected_cache))
        self.assertEqual(manifest["cmake_cache_sha256"], expected_hash)
        self.assertEqual(manifest["cocoa_plugin"], str(plugins / "platforms/libqcocoa.dylib"))

    def test_selected_debug_build_is_rejected_despite_sibling_release_cache(self):
        self.build("snow-shot-macos-arm64-performance")
        binary, _, plugins = self.build("snow-shot-macos-x64-debug", configuration="Debug")
        with self.assertRaisesRegex(RuntimeError, "Release"):
            self.invoke(binary, plugins)
        self.process_run.assert_not_called()
        self.assertFalse((self.output / "manifest.json").exists())

    def test_missing_selected_cache_fails_clearly_before_process_start(self):
        self.build("snow-shot-macos-arm64-performance")
        binary, _, plugins = self.build("snow-shot-macos-x64-performance", cache=False)
        with self.assertRaises(RuntimeError) as error:
            self.invoke(binary, plugins, "--qt-plugin-directory", str(plugins))
        self.assertIn("CMakeCache.txt", str(error.exception))
        self.assertIn(str(binary), str(error.exception))
        self.process_run.assert_not_called()
        self.assertFalse((self.output / "manifest.json").exists())

    def test_binary_symlink_uses_the_target_build(self):
        binary, cache, plugins = self.build("snow-shot-macos-x64-performance")
        link = self.repo / "shortcuts/benchmark"
        link.parent.mkdir()
        try:
            link.symlink_to(binary)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"Host cannot create a symlink: {error}")
        manifest = self.invoke(link, plugins)
        self.assertEqual(manifest["binary"], str(binary))
        self.assertEqual(manifest["cmake_cache"], str(cache))

    def test_plugin_override_is_preserved_after_selected_cache_qualification(self):
        binary, cache, _ = self.build("snow-shot-macos-x64-performance")
        _, override_plugins = self.plugin_kit("explicit-override")
        manifest = self.invoke(binary, override_plugins,
                               "--qt-plugin-directory", str(override_plugins))
        self.assertEqual(manifest["cmake_cache"], str(cache))
        self.assertEqual(manifest["cocoa_plugin"],
                         str(override_plugins / "platforms/libqcocoa.dylib"))


if __name__ == "__main__":
    unittest.main()
