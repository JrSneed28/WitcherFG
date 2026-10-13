"""Release boundaries: hostile archives, stale identity, and exact acceptance.

Fixtures never execute a binary. Native CLI checks use WFG_LAUNCHER separately.
"""
import copy
import importlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
try:
    release = importlib.import_module("tools.release")
except ModuleNotFoundError:
    release = None


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(release, "release renderer/packager is not implemented")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        for name in ("docs", "config", "third_party"):
            shutil.copytree(ROOT / name, self.root / name,
                            ignore=shutil.ignore_patterns("superpowers", "bug-reports", "toml.hpp"))
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "CHANGELOG.md"):
            shutil.copyfile(ROOT / name, self.root / name)
        self.metadata = release.load_metadata(ROOT / "release/project.toml")
        self.identity = {"version": "0.1.0-preview.1", "channel": "preview", "build_type": "Release",
                         "commit": "a" * 40, "dirty": False, "archive": False,
                         "diagnostic_schema": 1, "input_digest": "b" * 64, "commit_epoch": 1791676801}
        self.binary = self.root / "WitcherFG.exe"
        self.binary.write_bytes(b"MZ-fixture-only-not-an-executable")
        self.docs = release.render_documents(self.metadata, self.root / "docs")
        self.archive = self.root / "candidate.zip"

    def package(self):
        return release.make_install_archive(self.identity, self.binary, self.docs, self.archive)

    def rewrite(self, change):
        with zipfile.ZipFile(self.package()) as archive:
            infos = {entry.filename: entry for entry in archive.infolist()}
            payload = [(entry.filename, archive.read(entry)) for entry in archive.infolist()]
        payload = change(payload)
        with zipfile.ZipFile(self.archive, "w", zipfile.ZIP_STORED) as archive:
            for name, data in payload:
                info = copy.copy(infos.get(name, next(iter(infos.values()))))
                info.filename = name
                with warnings.catch_warnings():
                    warnings.filterwarnings("ignore", message="Duplicate name:", category=UserWarning)
                    archive.writestr(info, data)

    def test_installer_inventory_hashes_and_no_private_payload(self):
        with zipfile.ZipFile(self.package()) as archive:
            self.assertEqual(set(archive.namelist()), {
                "WitcherFG.exe", "README.txt", "VERSION.txt", "FrameGenFix.toml", "CHANGELOG.txt",
                "THIRD_PARTY_NOTICES.txt", "TROUBLESHOOTING.txt", "UNINSTALL.txt", "LICENSE",
                "licenses/tomlplusplus-LICENSE.txt", "SHA256SUMS"})
            self.assertIsNone(archive.testzip())
            self.assertIn(b"MIT License", archive.read("licenses/tomlplusplus-LICENSE.txt"))
            self.assertIn(b"source ZIP", archive.read("README.txt"))
        self.assertEqual(release.verify_release(self.archive, self.identity, None), [])

    def test_same_inputs_have_identical_bytes_and_fixed_zip_metadata(self):
        before = self.package().read_bytes()
        self.docs = dict(reversed(list(self.docs.items())))
        self.assertEqual(before, self.package().read_bytes())
        with zipfile.ZipFile(self.archive) as archive:
            self.assertEqual(archive.namelist(), sorted(archive.namelist()))
            for item in archive.infolist():
                self.assertEqual(item.compress_type, zipfile.ZIP_STORED)
                self.assertEqual(item.date_time, (2026, 10, 11, 0, 0, 0))
                self.assertEqual(item.extra, b"")
                self.assertEqual(item.comment, b"")

    def test_txt_encoding_and_crlf_are_normalized(self):
        with zipfile.ZipFile(self.package()) as archive:
            for name in archive.namelist():
                if name.endswith(".txt"):
                    text = archive.read(name)
                    text.decode("utf-8")
                    self.assertFalse(text.startswith(b"\xef\xbb\xbf"))
                    self.assertNotIn(b"\n", text.replace(b"\r\n", b""))
                    self.assertTrue(text.endswith(b"\r\n"))

    def test_dirty_archive_and_malformed_identities_refuse_packaging(self):
        for key, value in (("dirty", True), ("archive", True), ("commit", ""),
                           ("input_digest", "bad"), ("build_type", "Debug"), ("dirty", "false")):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                release.make_install_archive({**self.identity, key: value}, self.binary, self.docs, self.archive)

    def test_extra_or_missing_payload_is_refused(self):
        for docs in ({**self.docs, "sl.interposer.dll": b"private"},
                     {key: data for key, data in self.docs.items() if key != "LICENSE"}):
            with self.assertRaises(ValueError):
                release.make_install_archive(self.identity, self.binary, docs, self.archive)

    def test_missing_or_changed_dependency_license_is_refused(self):
        license_file = self.root / "third_party/tomlplusplus/LICENSE"
        license_file.write_text("wrong license", encoding="utf-8")
        with self.assertRaises(ValueError):
            release.render_documents(self.metadata, self.root / "docs")

    def test_invalid_config_default_is_refused(self):
        path = self.root / "config/FrameGenFix.toml"
        path.write_text(path.read_text().replace('level = "error"', 'level = "debug"'), encoding="utf-8")
        with self.assertRaises(ValueError):
            release.render_documents(self.metadata, self.root / "docs")

    def test_hostile_archive_names_duplicates_extras_and_hash_tampering(self):
        changes = [lambda p: p + [("../private.txt", b"x")], lambda p: p + [p[0]],
                   lambda p: p + [("sl.interposer.dll", b"x")],
                   lambda p: [(n, d + b"x" if n == "WitcherFG.exe" else d) for n, d in p]]
        for change in changes:
            with self.subTest(change=changes.index(change)):
                self.rewrite(change)
                # The first change plants "../private.txt", a path-traversal entry, in the shipped
                # archive. Asserting only that *some* error occurred would keep passing if the name
                # allowlist were dropped and an unrelated check happened to fire.
                self.assertEqual(release.verify_release(self.archive, self.identity, None),
                                 ([["archive names duplicate, unsafe, missing or outside allowlist"]] * 3 +
                                  [["payload SHA256 manifest mismatch"]])[changes.index(change)])

    def test_executable_tampering_reaches_payload_sha_check(self):
        self.rewrite(lambda p: [(n, d + b"x" if n == "WitcherFG.exe" else d) for n, d in p])
        self.assertEqual(release.verify_release(self.archive, self.identity, None),
                         ["payload SHA256 manifest mismatch"])

    def test_corrupt_zip_is_a_verification_error(self):
        self.archive.write_bytes(b"not a zip")
        self.assertEqual(release.verify_release(self.archive, self.identity, None),
                         ["archive verification failed: File is not a zip file"])

    def test_symlink_and_self_consistent_wrong_license_refuse_verification(self):
        self.package()
        with zipfile.ZipFile(self.archive) as archive:
            entries = [(entry, archive.read(entry)) for entry in archive.infolist()]
        with zipfile.ZipFile(self.archive, "w") as archive:
            for entry, data in entries:
                if entry.filename == "WitcherFG.exe":
                    entry.external_attr = 0o120777 << 16
                archive.writestr(entry, data)
        # The symlink must be refused *as an entry type*. Truthiness also passed when the
        # external-attribute check was absent and some unrelated error fired instead.
        self.assertEqual(release.verify_release(self.archive, self.identity, None),
                         ["unsupported archive entry type, bounds or compression"])

    def test_verifier_refuses_rehashed_wrong_license_config_and_stable_claim(self):
        for name, bad in (("LICENSE", b"wrong license\n"), ("FrameGenFix.toml", b"schema_version=2\n"),
                          ("README.txt", b"Validated stable release\r\n")):
            with self.subTest(name=name):
                self.package()
                with zipfile.ZipFile(self.archive) as archive:
                    entries = [(item, archive.read(item)) for item in archive.infolist()]
                payload = {item.filename: data for item, data in entries}
                payload[name] = bad
                payload["SHA256SUMS"] = ''.join(release.sha256(payload[n]) + '  ' + n + '\n'
                    for n in sorted(payload) if n != 'SHA256SUMS').encode('ascii')
                with zipfile.ZipFile(self.archive, "w") as archive:
                    for item, _ in entries:
                        archive.writestr(item, payload[item.filename])
                self.assertEqual(release.verify_release(self.archive, self.identity, None),
                                 [{"LICENSE": "project license mismatch",
                                   "FrameGenFix.toml": "canonical default configuration mismatch",
                                   "README.txt": "candidate/version/source ZIP warning missing"}[name]])

    def validation(self):
        return {"schema": 1, "version": self.identity["version"],
                "artifact_sha256": release.sha256(self.archive.read_bytes()),
                "binary_sha256": release.sha256(self.binary.read_bytes()),
                "source_commit": self.identity["commit"], "input_digest": self.identity["input_digest"],
                "build_id": self.identity["input_digest"], "tag": "", "publication_ready": True,
                "environment": {"windows": "fixture", "gpu": "fixture", "driver": "fixture",
                                "game_package": "fixture", "method": "fixture only"},
                "known_limits": ["Synthetic fixture; never actual acceptance"],
                "cases": {name: {"status": "Passed", "evidence": "fixture-record"}
                          for name in ("fresh_install", "upgrade_config", "gameplay_fg", "guard_refusal", "uninstall_restore")}}

    def test_acceptance_matches_exact_archive_binary_and_source(self):
        self.package()
        record = self.validation()
        self.assertEqual(release.verify_release(self.archive, self.identity, record), [])
        for field in ("artifact_sha256", "binary_sha256", "source_commit", "input_digest", "build_id", "version"):
            with self.subTest(field=field):
                changed = {**record, field: "mismatch"}
                self.assertTrue(release.verify_release(self.archive, self.identity, changed))
        pending = copy.deepcopy(record)
        pending["cases"]["gameplay_fg"]["status"] = "Pending"
        self.assertTrue(release.verify_release(self.archive, self.identity, pending))
        changed = {**record, "github_sha256": record["artifact_sha256"], "nexus_sha256": "c" * 64}
        self.assertTrue(release.verify_release(self.archive, self.identity, changed))

    def test_documentation_only_acceptance_does_not_change_archive(self):
        self.package()
        before = self.archive.read_bytes()
        record = self.validation()
        (self.root / "docs/acceptance.md").write_text(record["artifact_sha256"], encoding="utf-8")
        self.assertEqual(release.verify_release(self.archive, self.identity, record), [])
        self.assertEqual(self.archive.read_bytes(), before)

    def git_fixture(self):
        """Real CLI/Git provenance with fixture bytes; never an executable or acceptance."""
        repo = self.root / "git-source"
        repo.mkdir()
        for name in ("docs", "config", "third_party", "tools", "release"):
            shutil.copytree(ROOT / name, repo / name, ignore=shutil.ignore_patterns(
                "superpowers", "bug-reports", "__pycache__", "validation.json"))
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "CHANGELOG.md"):
            shutil.copyfile(ROOT / name, repo / name)
        (repo / "src").mkdir()
        (repo / "src/probe.cpp").write_text("// synthetic build-input fixture\n", encoding="utf-8")
        self.fixture_repo = repo
        self.fixture_git("init")
        self.fixture_git("add", ".")
        self.fixture_git("commit", "-m", "synthetic fixture source")
        commit = self.fixture_git("rev-parse", "HEAD").strip()
        self.identity = release.build_identity.make_identity(self.metadata,
            {"commit": commit, "dirty": False, "archive": False,
             "inputs": release.build_identity._collect_inputs(repo)}, "Release")
        self.identity["commit_epoch"] = int(self.fixture_git("show", "-s", "--format=%ct", "HEAD"))
        self.package()
        self.archive.with_suffix(".manifest.json").write_text(json.dumps({
            "build": self.identity, "artifact_sha256": release.sha256(self.archive.read_bytes()),
            "binary_sha256": release.sha256(self.binary.read_bytes())}), encoding="utf-8")
        return repo, commit

    def fixture_git(self, *arguments):
        result = subprocess.run(["git", "-c", "user.name=Fixture", "-c", "user.email=fixture@invalid.test",
            "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false", "-c", "core.autocrlf=false",
            *arguments], cwd=self.fixture_repo,
            text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def public_verify(self, record=None):
        arguments = [sys.executable, str(self.fixture_repo / "tools/release.py"),
                     "verify", "--archive", str(self.archive)]
        if record is not None:
            validation = self.root / "synthetic-validation.json"
            validation.write_text(json.dumps(record), encoding="utf-8")
            arguments += ["--validation", str(validation)]
        return subprocess.run(arguments, cwd=self.fixture_repo, text=True, capture_output=True, timeout=30)

    def test_public_verifier_rejects_nonexistent_acceptance_tag(self):
        self.git_fixture()
        tag = "v" + self.identity["version"]
        result = self.public_verify({**self.validation(), "tag": tag})
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("acceptance tag", result.stderr)
        self.fixture_git("branch", tag)
        result = self.public_verify({**self.validation(), "tag": tag})
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("acceptance tag", result.stderr)

    def test_public_verifier_rejects_acceptance_tag_with_changed_inputs_or_metadata(self):
        repo, tested_commit = self.git_fixture()
        tag = "v" + self.identity["version"]
        for kind in ("code", "metadata"):
            with self.subTest(kind=kind):
                if kind == "code":
                    (repo / "src/probe.cpp").write_text("// changed code, not tested\n", encoding="utf-8")
                else:
                    path = repo / "release/project.toml"
                    path.write_text(path.read_text().replace('diagnostic_schema = 1', 'diagnostic_schema = 2'),
                                    encoding="utf-8")
                self.fixture_git("add", ".")
                self.fixture_git("commit", "-m", "synthetic untested " + kind)
                self.fixture_git("tag", tag)
                self.fixture_git("checkout", "--detach", tested_commit)
                result = self.public_verify({**self.validation(), "tag": tag})
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("acceptance tag", result.stderr)
                self.fixture_git("tag", "-d", tag)

    def test_public_verifier_accepts_annotated_documentation_only_tag_without_repackaging(self):
        repo, tested_commit = self.git_fixture()
        before = self.archive.read_bytes()
        record = {**self.validation(), "tag": "v" + self.identity["version"]}
        (repo / "docs/acceptance.md").write_text("Synthetic fixture only: " + record["artifact_sha256"],
                                                 encoding="utf-8")
        self.fixture_git("add", ".")
        self.fixture_git("commit", "-m", "synthetic documentation-only acceptance")
        tagged_commit = self.fixture_git("rev-parse", "HEAD").strip()
        self.assertNotEqual(tagged_commit, tested_commit)
        self.fixture_git("tag", "-a", record["tag"], "-m", "synthetic fixture tag")
        self.assertEqual(self.fixture_git("rev-parse", "refs/tags/" + record["tag"] + "^{commit}").strip(),
                         tagged_commit)
        result = self.public_verify(record)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("external live acceptance matches", result.stdout)
        self.assertEqual(self.archive.read_bytes(), before)

    def test_public_verifier_accepts_untagged_pending_preview(self):
        self.git_fixture()
        result = self.public_verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("candidate; live acceptance Pending", result.stdout)
        result = self.public_verify(self.validation())
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_stable_metadata_cannot_create_unaccepted_archive(self):
        with self.assertRaises(ValueError):
            release.render_documents({**self.metadata, "channel": "stable"}, self.root / "docs")

    def test_supported_markdown_and_bbcode_escape_literal_markup(self):
        text = "# Title\n\nA **bold** [link](https://github.com/actions) and `[url=evil]`.\n\n- First\n- Second\n\n```text\n[value]\n```\n\n| A | B |\n| --- | --- |\n| x | y |\n"
        plain = release.render_markdown(text, "text")
        bbcode = release.render_markdown(text, "bbcode")
        self.assertIn("link (https://github.com/actions)", plain)
        self.assertIn("[b]Title[/b]", bbcode)
        self.assertNotIn("[url=evil]", bbcode)
        for hostile in ("<script>x</script>", "<script/x>", "- a\n  - nested", "![image](x.png)", "# Heading\n\n```\nunterminated", "> quote"):
            with self.subTest(hostile=hostile), self.assertRaises(ValueError):
                release.render_markdown(hostile, "bbcode")

    def test_toolchain_record_contains_actual_compiler_sdk_and_cmake_version(self):
        build = self.root / "build"
        build.mkdir()
        (build / "CMakeCache.txt").write_text(
            'CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n'
            'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\n'
            'CMAKE_COMPILE_WARNING_AS_ERROR:BOOL=ON\n', encoding="utf-8")
        generated = build / "CMakeFiles/4.2.0"
        generated.mkdir(parents=True)
        (generated / "CMakeCXXCompiler.cmake").write_text('set(CMAKE_CXX_COMPILER_VERSION "19.51.36260.0")\n', encoding="utf-8")
        (build / "WitcherFG.vcxproj").write_text('<Project><WindowsTargetPlatformVersion>10.0.28000.0</WindowsTargetPlatformVersion></Project>', encoding="utf-8")
        actual = release.record_toolchain(build)
        self.assertEqual(actual["msvc"], "19.51.36260.0")
        self.assertEqual(actual["windows_sdk"], "10.0.28000.0")
        self.assertEqual(actual["cmake"], "4.2.0")
        # The manifest must be able to evidence the warning policy; input_digest cannot, because
        # it covers source bytes only and is identical for a /WX and a non-/WX build.
        self.assertEqual(actual["CMAKE_COMPILE_WARNING_AS_ERROR"], "ON")
        self.assertIs(actual["warning_as_error"], True)

    def test_toolchain_record_reports_warning_policy_absent_or_off(self):
        for cache, expected in (("", False), ("CMAKE_COMPILE_WARNING_AS_ERROR:BOOL=OFF\n", False)):
            with self.subTest(cache=cache):
                build = self.root / ("build-" + str(len(cache)))
                build.mkdir()
                (build / "CMakeCache.txt").write_text(
                    'CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026\n' + cache, encoding="utf-8")
                generated = build / "CMakeFiles/4.2.0"
                generated.mkdir(parents=True)
                (generated / "CMakeCXXCompiler.cmake").write_text(
                    'set(CMAKE_CXX_COMPILER_VERSION "19.51.36260.0")\n', encoding="utf-8")
                (build / "WitcherFG.vcxproj").write_text(
                    '<Project><WindowsTargetPlatformVersion>10.0.28000.0</WindowsTargetPlatformVersion></Project>',
                    encoding="utf-8")
                self.assertIs(release.record_toolchain(build)["warning_as_error"], expected)

    def test_tracked_render_check_detects_drift_without_writing(self):
        shutil.copyfile(ROOT / "README.md", self.root / "README.md")
        release.render_tracked(self.metadata, self.root, check=False)
        self.assertEqual(release.render_tracked(self.metadata, self.root, check=True), [])
        target = self.root / "docs/NEXUS_DESCRIPTION.bbcode"
        target.write_text("stale", encoding="utf-8")
        self.assertIn("docs/NEXUS_DESCRIPTION.bbcode", release.render_tracked(self.metadata, self.root, check=True))
        self.assertEqual(target.read_text(), "stale")

    def test_channel_outputs_preserve_scope_version_and_unpublished_links(self):
        shutil.copyfile(ROOT / "README.md", self.root / "README.md")
        release.render_tracked(self.metadata, self.root, check=False)
        text = (self.root / "docs/NEXUS_DESCRIPTION.bbcode").read_text()
        self.assertIn("0.1.0-preview.1", text)
        self.assertIn("Verified: diagnostic probe", text)
        self.assertIn("Pending", text)
        self.assertIn("unpublished", text)
        self.assertNotIn("github.com/OWNER", text)

    def test_executable_contract_compares_all_fields_and_refuses_malformed(self):
        generated = {k: v for k, v in self.identity.items() if k != "commit_epoch"}
        text = ("WitcherFG 0.1.0-preview.1 Release commit=" + "a" * 40 + " input=" + "b" * 64 +
                " channel=preview dirty=false archive=false diagnostic_schema=1\nCompleted UTC: fixture\n")
        self.assertEqual(release.parse_version(text), generated)
        release.check_identity(generated, generated, self.metadata, "b" * 64)
        for key, value in (("dirty", True), ("archive", True), ("channel", "stable"), ("diagnostic_schema", 2)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                release.check_identity({**generated, key: value}, generated, self.metadata, "b" * 64)
        with self.assertRaises(ValueError):
            release.check_identity(generated, generated, self.metadata, "c" * 64)
        for value in (text + text, text.replace(" dirty=false", ""), text.replace("dirty=false", "dirty=0")):
            with self.assertRaises(ValueError):
                release.parse_version(value)


if __name__ == "__main__":
    unittest.main()
