"""Fail-closed guard for AR-05.d: the mutation cycle may only touch an isolated copy.

The AR-05.d failure-injection test deliberately changes the Gain kernel coefficient.
That change must never reach the active working tree or the main build's binaries,
because another agent, build or the user may be using them concurrently.
"""
from __future__ import annotations

import hashlib
import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tests" / "infrastructure" / "test_ar05_gain_mutation.py"
MAIN_BUILD = ROOT / "build" / "windows-debug"


def load_module():
    spec = importlib.util.spec_from_file_location("ar05d_mutation", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class MutationIsolationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = load_module()
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_active_gain_is_read_only_and_never_a_write_target(self):
        active = Path(self.module.ACTIVE_GAIN).resolve()
        self.assertEqual(active, (ROOT / "libs" / "audio_algorithms" / "Gain.cpp").resolve())
        self.assertTrue(active.is_file())
        for forbidden in ("ACTIVE_GAIN.write", "ACTIVE_GAIN.unlink", "SOURCE.write", "SOURCE.unlink"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.text)
        # The mutation anchor must be applied to a copy, never to the active file.
        self.assertNotIn('SOURCE = ROOT / "libs"', self.text)

    def test_isolation_guard_rejects_active_source_and_main_build(self):
        guard = self.module.assert_isolated
        with self.assertRaises(RuntimeError):
            guard(self.module.ACTIVE_GAIN, ROOT / "build" / "reports" / "isolated", [MAIN_BUILD])
        with self.assertRaises(RuntimeError):
            guard(MAIN_BUILD / "bin" / "audioalgo_tests.exe", MAIN_BUILD, [MAIN_BUILD])
        with self.assertRaises(RuntimeError):
            guard(self.module.ACTIVE_GAIN.parent / "copied.cpp", self.module.ACTIVE_GAIN.parent, [MAIN_BUILD])
        isolated = ROOT / "build" / "reports" / "architecture_refactor" / "ar05d_mutation" / "probe"
        self.assertEqual(
            guard(isolated / "libs" / "audio_algorithms" / "Gain.cpp", isolated, [MAIN_BUILD]),
            (isolated / "libs" / "audio_algorithms" / "Gain.cpp").resolve())

    def test_source_copy_leaves_the_active_tree_byte_identical(self):
        before = digest(self.module.ACTIVE_GAIN)
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "source"
            copied = self.module.copy_source_tree(destination)
            copied_gain = Path(copied) / "libs" / "audio_algorithms" / "Gain.cpp"
            self.assertTrue(copied_gain.is_file())
            self.assertEqual(digest(copied_gain), before)
            self.assertTrue((Path(copied) / "CMakeLists.txt").is_file())
            self.assertTrue((Path(copied) / "plugins" / "builtin" / "registration.cpp").is_file())
            self.assertEqual(digest(Path(copied) / "sdk/include/audioalgo/node_abi_v1.h"),
                             digest(ROOT / "sdk/include/audioalgo/node_abi_v1.h"))
            self.assertFalse((Path(copied) / "build").exists())
        self.assertEqual(digest(self.module.ACTIVE_GAIN), before)

    def test_workspace_snapshot_covers_active_source_and_main_binaries(self):
        snapshot = self.module.workspace_snapshot(ROOT, MAIN_BUILD)
        self.assertIn("libs/audio_algorithms/Gain.cpp", snapshot)
        self.assertTrue(snapshot["libs/audio_algorithms/Gain.cpp"])
        for name in ("audio_algorithm_tests.exe", "audioalgo_tests.exe", "audio_cli.exe"):
            key = f"bin/{name}"
            with self.subTest(binary=name):
                self.assertIn(key, snapshot, f"main build binary is missing: {name}")
        self.assertEqual(snapshot, self.module.workspace_snapshot(ROOT, MAIN_BUILD))

    def test_isolated_configure_never_targets_the_main_build(self):
        arguments = self.module.isolated_configure_arguments(
            source_root=ROOT / "build" / "reports" / "isolated" / "source",
            build_root=ROOT / "build" / "reports" / "isolated" / "build",
            juce_source=None, catch2_source=None)
        self.assertIn("-B", arguments)
        target = Path(arguments[arguments.index("-B") + 1]).resolve()
        self.assertNotEqual(target, MAIN_BUILD.resolve())
        self.assertNotIn(MAIN_BUILD.resolve(), target.parents)
        self.assertTrue(str(target).startswith(str((ROOT / "build" / "reports").resolve())))
        self.assertIn("-DAUDIO_BUILD_EDITOR=OFF", arguments)
        self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", arguments)

    def test_parallel_runs_get_unique_roots_and_never_delete_each_other(self):
        # F4: the isolation root must be created atomically and uniquely, and an existing
        # directory belonging to another run must never be removed.
        parent = ROOT / "build" / "ar05d"
        parent.mkdir(parents=True, exist_ok=True)
        create = self.module.create_isolation_root
        first = create(parent)
        second = create(parent)
        self.assertNotEqual(Path(first).resolve(), Path(second).resolve())
        self.assertTrue(str(Path(first).resolve()).startswith(str(parent.resolve())))
        for created in (first, second):
            self.assertTrue((Path(created) / "marker.txt").parent.is_dir(), created)
            (Path(created) / "marker.txt").write_text("own", encoding="utf-8")

        # A third run reusing the same parent must leave both markers intact.
        third = create(parent)
        self.assertNotIn(Path(third).resolve(), {Path(first).resolve(), Path(second).resolve()})
        self.assertEqual((Path(first) / "marker.txt").read_text(encoding="utf-8"), "own")
        self.assertEqual((Path(second) / "marker.txt").read_text(encoding="utf-8"), "own")

        # Passing an existing directory must be refused instead of deleted.
        with self.assertRaises(RuntimeError):
            self.module.reserve_isolation_root(parent, Path(first).name)
        self.assertEqual((Path(first) / "marker.txt").read_text(encoding="utf-8"), "own")
        for created in (first, second, third):
            shutil.rmtree(Path(created), ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
