"""AR-06.d dependency and composition checks plus the registered catalog behavior test."""
from __future__ import annotations

import argparse
import subprocess
import unittest
from pathlib import Path


class PluginBoundaryTests(unittest.TestCase):
    root: Path

    def read(self, relative: str) -> str:
        return (self.root / relative).read_text(encoding="utf-8")

    def test_sdk_does_not_own_builtin_registration(self) -> None:
        sdk = self.read("libs/audio_node_sdk/NodeSDK.h") + self.read("libs/audio_node_sdk/NodeSDK.cpp")
        self.assertNotIn("register_builtin_node_descriptors", sdk)
        self.assertNotIn('"builtin.gain"', sdk)

    def test_backend_has_no_gain_or_builtin_factory_branch(self) -> None:
        backend = "\n".join(self.read(path) for path in (
            "libs/audio_juce_backend/JuceGraph.h",
            "libs/audio_juce_backend/JuceGraph.cpp",
            "libs/audio_juce_backend/NativeNodeAudioProcessor.h",
            "libs/audio_juce_backend/NativeNodeAudioProcessor.cpp",
        ))
        for forbidden in ("JuceProcessorRegistry", "register_builtin_juce_processors", "GainAudioProcessor",
                          "GainProcessor", "GainNodeAdapter", "audio_algorithms/Gain.h", '"builtin.gain"'):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, backend)

    def test_backend_link_interface_excludes_algorithm_and_builtin_plugin(self) -> None:
        cmake = self.read("CMakeLists.txt")
        backend_links = cmake.split("target_link_libraries(audio_juce_backend", 1)[1].split(")", 1)[0]
        self.assertNotIn("audio_algorithms", backend_links)
        self.assertNotIn("audio_nodes_builtin", backend_links)
        plugin_links = cmake.split("target_link_libraries(audio_nodes_builtin", 1)[1].split(")", 1)[0]
        self.assertIn("audio_algorithms", plugin_links)
        self.assertIn("audio_node_sdk", plugin_links)

    def test_cli_and_editor_share_plugin_composition(self) -> None:
        plugin = self.read("plugins/builtin/registration.h") + self.read("plugins/builtin/registration.cpp")
        self.assertIn("register_builtin_nodes", plugin)
        for app in ("apps/cli/main.cpp", "apps/editor/Main.cpp"):
            source = self.read(app)
            self.assertIn("plugins/builtin/registration.h", source)
            self.assertIn("register_builtin_nodes", source)
            self.assertNotIn("register_builtin_node_descriptors", source)
            self.assertNotIn("register_builtin_juce_processors", source)

    def test_editor_view_helpers_receive_only_read_only_catalog(self) -> None:
        for path in ("apps/editor/EditorGeometry.h", "apps/editor/EditorNodeCreation.h",
                     "apps/editor/EditorNodeDragDrop.h", "apps/editor/EditorTopologyCommands.h"):
            source = self.read(path)
            self.assertIn("NodeCatalog", source, path)
            self.assertNotIn("NodeRegistry", source, path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    args = parser.parse_args()
    PluginBoundaryTests.root = args.source_root.resolve()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(PluginBoundaryTests))
    if not result.wasSuccessful():
        return 1

    test = subprocess.run([str(args.executable.resolve()),
                           "AR-06.d plugin composition uses one catalog and exposes metadata read-only"],
                          cwd=args.executable.resolve().parent,
                          capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    print(test.stdout, end="")
    print(test.stderr, end="")
    if test.returncode != 0:
        print(f"registered runtime composition test exited with {test.returncode}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
