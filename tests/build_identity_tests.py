import importlib.util
import argparse
import json
import pathlib
import tempfile
import subprocess
import sys
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "tools" / "build_identity.py"
SPEC = importlib.util.spec_from_file_location("build_identity", SCRIPT)
build_identity = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_identity)
NATIVE_RUNNER = None
GENERATED_JSON = None


class BuildIdentityTests(unittest.TestCase):
    def setUp(self):
        self.metadata = {
            "version": "0.1.0-preview.1",
            "channel": "preview",
            "diagnostic_schema": 1,
            "support_url": "",
            "release_url": "",
        }
        self.git = {
            "commit": "a" * 40,
            "dirty": False,
            "archive": False,
            "inputs": {"src/configuration.cpp": "config-v1", "CMakeLists.txt": "build-v1"},
        }

    def test_identity_contains_declared_metadata_and_build_state(self):
        identity = build_identity.make_identity(self.metadata, self.git, "Release")
        self.assertEqual(identity["version"], self.metadata["version"])
        self.assertEqual(identity["channel"], "preview")
        self.assertEqual(identity["build_type"], "Release")
        self.assertEqual(identity["commit"], "a" * 40)
        self.assertFalse(identity["dirty"])
        self.assertFalse(identity["archive"])
        self.assertEqual(identity["diagnostic_schema"], 1)
        self.assertTrue(identity["input_digest"])

    def test_documentation_and_publication_metadata_do_not_change_digest(self):
        original = build_identity.make_identity(self.metadata, self.git, "Release")
        changed_metadata = dict(self.metadata, support_url="https://example.invalid", release_url="https://example.invalid")
        changed_git = dict(self.git, inputs=dict(self.git["inputs"], **{"README.md": "doc-v2", "docs/release.md": "release-v2"}))
        changed = build_identity.make_identity(changed_metadata, changed_git, "Release")
        self.assertEqual(original["input_digest"], changed["input_digest"])

    def test_code_and_build_inputs_change_digest(self):
        original = build_identity.make_identity(self.metadata, self.git, "Release")
        changed_git = dict(self.git, inputs=dict(self.git["inputs"], **{"src/configuration.cpp": "config-v2"}))
        changed_code = build_identity.make_identity(self.metadata, changed_git, "Release")
        changed_build = build_identity.make_identity(self.metadata, self.git, "Debug")
        self.assertNotEqual(original["input_digest"], changed_code["input_digest"])
        self.assertNotEqual(original["input_digest"], changed_build["input_digest"])
        for changed_metadata in (
            dict(self.metadata, version="0.1.0-preview.2"),
            dict(self.metadata, channel="stable"),
            dict(self.metadata, diagnostic_schema=2),
        ):
            changed = build_identity.make_identity(changed_metadata, self.git, "Release")
            self.assertNotEqual(original["input_digest"], changed["input_digest"])

    def test_source_archive_identity_is_honest_and_header_matches_json(self):
        archive_git = {"commit": "", "dirty": False, "archive": True, "inputs": self.git["inputs"]}
        identity = build_identity.make_identity(self.metadata, archive_git, "Release")
        self.assertEqual(identity["commit"], "")
        self.assertTrue(identity["archive"])
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / "build_info.hpp"
            build_identity.write_header(identity, output)
            metadata_path = output.with_suffix(".json")
            self.assertEqual(json.loads(metadata_path.read_text()), identity)
            header = output.read_text()
            for key in ("version", "channel", "build_type", "input_digest"):
                self.assertIn(identity[key], header)

    def test_dirty_state_is_preserved_and_missing_commit_marks_archive(self):
        dirty_git = dict(self.git, dirty=True)
        dirty = build_identity.make_identity(self.metadata, dirty_git, "Release")
        self.assertTrue(dirty["dirty"])
        archive = build_identity.make_identity(self.metadata, {"inputs": self.git["inputs"]}, "Release")
        self.assertTrue(archive["archive"])
        self.assertEqual(archive["commit"], "")

    def test_vendor_license_and_manifest_change_the_digest(self):
        original = build_identity.make_identity(self.metadata, self.git, "Release")
        for name in ("third_party/tomlplusplus/LICENSE", "third_party/dependencies.toml", "third_party/tomlplusplus/toml.hpp"):
            with self.subTest(name=name):
                changed_git = dict(self.git, inputs=dict(self.git["inputs"], **{name: "vendor content"}))
                changed = build_identity.make_identity(self.metadata, changed_git, "Release")
                self.assertNotEqual(original["input_digest"], changed["input_digest"])

    def test_compiled_identity_matches_generated_json(self):
        if not NATIVE_RUNNER or not GENERATED_JSON:
            self.skipTest("native runner and generated JSON were not supplied")
        result = subprocess.run([str(NATIVE_RUNNER), "--filter", "diagnostics.embedded_identity_matches_generated_build"], check=True, capture_output=True, text=True)
        line = next(line for line in result.stdout.splitlines() if line.startswith("WFG_BUILD_IDENTITY:"))
        compiled = json.loads(line.removeprefix("WFG_BUILD_IDENTITY:"))["build"]
        self.assertEqual(compiled, json.loads(GENERATED_JSON.read_text(encoding="utf-8")))


class NativeConfigurationTests(unittest.TestCase):
    def test_snapshot_uses_real_selected_file_defaults_and_origins(self):
        if not NATIVE_RUNNER:
            self.skipTest("native runner was not supplied")
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            selected = root / "selected.toml"
            (root / "FrameGenFix.toml").write_text("schema_version='hostile cwd'", encoding="utf-8")
            absent = subprocess.run([str(NATIVE_RUNNER), "--config-snapshot", str(selected)], cwd=root, capture_output=True, text=True)
            self.assertEqual(absent.returncode, 0, absent.stderr)
            defaults = json.loads(absent.stdout)
            self.assertEqual(defaults["schema_version"], 1)
            self.assertEqual(defaults["logging"], {"level": "error", "file_logging": True, "max_file_mib": 2, "retained_files": 5})
            self.assertEqual(defaults["diagnostics"], {"runtime_observation": False, "sample_interval_ms": 1000, "observation_seconds": 30})
            self.assertEqual(len(defaults["origins"]), 8)
            self.assertEqual(set(defaults["origins"].values()), {"default"})
            selected.write_text("schema_version=1\n[logging]\nlevel='debug'\nmax_file_mib=8\n", encoding="utf-8")
            loaded = subprocess.run([str(NATIVE_RUNNER), "--config-snapshot", str(selected), "--log-level", "info"], cwd=root, capture_output=True, text=True)
            self.assertEqual(loaded.returncode, 0, loaded.stderr)
            config = json.loads(loaded.stdout)
            self.assertEqual(config["logging"]["level"], "info")
            self.assertEqual(config["logging"]["max_file_mib"], 8)
            self.assertEqual(config["origins"]["logging.level"], "command_line")
            self.assertEqual(config["origins"]["logging.max_file_mib"], "file")
            self.assertEqual(config["origins"]["logging.file_logging"], "default")
            for content in ("schema_version=1\n[logging]\nmax_file_mib=9\n", "schema_version=1\nschema_version=1\n", "schema_version=1\nmodule='unsafe'\n"):
                selected.write_text(content, encoding="utf-8")
                invalid = subprocess.run([str(NATIVE_RUNNER), "--config-snapshot", str(selected)], cwd=root, capture_output=True, text=True)
                self.assertNotEqual(invalid.returncode, 0)
                self.assertEqual(invalid.stdout, "")
                self.assertIn("CONFIG_INVALID", invalid.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--native-runner", type=pathlib.Path)
    parser.add_argument("--generated-json", type=pathlib.Path)
    options, remaining = parser.parse_known_args()
    NATIVE_RUNNER = options.native_runner
    GENERATED_JSON = options.generated_json
    unittest.main(argv=[sys.argv[0], *remaining])
