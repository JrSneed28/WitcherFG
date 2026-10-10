"""Public evidence/link contracts and integration with the real config parser.

Set WFG_TEST_RUNNER and WFG_LAUNCHER to native Windows executables to run
the integration cases. Portable checks never activate or inspect a game.
"""
import datetime
import json
import os
import pathlib
import re
import subprocess
import tempfile
import tomllib
import unittest
from urllib.parse import unquote, urlsplit

ROOT = pathlib.Path(__file__).resolve().parents[1]
PUBLIC_FILES = (
    "README.md", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md", "LICENSE",
    "THIRD_PARTY_NOTICES.md", "docs/installation.md", "docs/uninstall.md",
    "docs/configuration.md", "docs/diagnostics.md", "docs/troubleshooting.md",
    "docs/building.md", "docs/releasing.md", "docs/TECHNICAL.md",
    "docs/compatibility.md", "docs/compatibility.toml", "docs/release-summary.md",
    "docs/NEXUS_DESCRIPTION.bbcode", "config/FrameGenFix.toml",
    ".github/ISSUE_TEMPLATE/bug_report.yml", ".github/ISSUE_TEMPLATE/config.yml",
    ".editorconfig", ".gitattributes", ".gitleaks.toml",
)
STATUSES = {"Verified", "Untested", "Reported", "Pending", "Unsupported"}
SCOPES = {"diagnostic_probe", "native_fixture", "native_release"}
PRIVATE = ("evidence", "artifacts", "docs/superpowers", "docs/bug-reports",
           ".superpowers", ".codex", "build", "dist")


def validate_matrix(rows: list[dict]) -> list[str]:
    errors = []
    for index, row in enumerate(rows):
        prefix = f"row {index}: "
        for key in ("component", "status", "scope", "native_release_status",
                    "evidence_method", "evidence_date", "game_identity", "details"):
            if key not in row or not isinstance(row[key], str):
                errors.append(prefix + "missing/string field " + key)
        if row.get("status") not in STATUSES:
            errors.append(prefix + "invalid status")
        if row.get("scope") not in SCOPES:
            errors.append(prefix + "invalid evidence scope")
        if row.get("native_release_status") not in STATUSES:
            errors.append(prefix + "invalid native release status")
        if not row.get("component") or not row.get("details"):
            errors.append(prefix + "missing component/details")
        if row.get("status") == "Verified":
            if not row.get("evidence_method") or not row.get("game_identity"):
                errors.append(prefix + "Verified requires evidence and identity")
            try:
                datetime.date.fromisoformat(row.get("evidence_date", ""))
            except (ValueError, TypeError):
                errors.append(prefix + "Verified requires an evidence date")
        if row.get("scope") != "native_release" and row.get("native_release_status") == "Verified":
            errors.append(prefix + "probe/fixture cannot verify a native release")
        if row.get("native_release_status") == "Verified" or (
                row.get("scope") == "native_release" and row.get("status") == "Verified"):
            if row.get("status") != "Verified" or row.get("native_release_status") != "Verified":
                errors.append(prefix + "inconsistent release verification")
            if not re.fullmatch(r"[0-9a-f]{64}", row.get("artifact_sha256", "")):
                errors.append(prefix + "Verified release requires artifact SHA256")
            if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", row.get("build_id", "")):
                errors.append(prefix + "Verified release requires build ID")
    return errors


def format_matrix_status(row: dict) -> str:
    if row["status"] == "Verified":
        return "Verified: " + {"diagnostic_probe": "diagnostic probe",
                                "native_fixture": "native fixture",
                                "native_release": "native release"}[row["scope"]]
    return row["status"]


def validate_public_links(root: pathlib.Path) -> list[str]:
    errors = []
    files = [root / p for p in PUBLIC_FILES if p.endswith((".md", ".bbcode"))]
    files += list((root / "docs/validation").glob("*.md"))
    for path in files:
        if not path.exists():
            errors.append(f"missing public source: {path.relative_to(root)}")
            continue
        text = path.read_text(encoding="utf-8")
        destinations = re.findall(r"\[[^\]\n]+\]\(([^)\n]+)\)", text)
        destinations += re.findall(r"\[url=([^\]]+)\]", text, re.IGNORECASE)
        destinations += re.findall(r"^\[[^\]]+\]:\s*(\S+)", text, re.MULTILINE)
        for destination in destinations:
            destination = destination.strip().strip("<>")
            parsed = urlsplit(destination)
            if parsed.scheme:
                if parsed.scheme not in {"https", "http"} or re.search(
                        r"(?:example\.com|OWNER|REPO|<|>|placeholder)", destination, re.I):
                    errors.append(f"invalid/unpublished URL: {destination}")
                continue
            target = (path.parent / unquote(parsed.path)).resolve() if parsed.path else path.resolve()
            try:
                relative = target.relative_to(root.resolve()).as_posix()
            except ValueError:
                errors.append(f"link outside public repository: {destination}")
                continue
            if any(relative == p or relative.startswith(p + "/") for p in PRIVATE):
                errors.append(f"private link: {destination}")
            elif not target.exists():
                errors.append(f"missing link target: {destination}")
        if path.name == "README.md" and any(p + "/" in text for p in PRIVATE):
            errors.append("README mentions an ignored internal location")
    return errors


def read_config_snapshot(test_runner: pathlib.Path, config: pathlib.Path) -> dict:
    result = subprocess.run([str(test_runner), "--config-snapshot", str(config)],
                            capture_output=True, text=True, timeout=20, check=False)
    if result.returncode:
        raise ValueError(f"configuration refused ({result.returncode}): {result.stderr.strip()}")
    return json.loads(result.stdout)


def assert_version_identity(text: str, generated_identity: dict | None = None) -> dict:
    import sys
    sys.path.insert(0, str(ROOT))
    from tools.release import parse_version
    try:
        fields = parse_version(text)
    except ValueError as error:
        raise AssertionError(str(error)) from error
    if generated_identity is not None:
        for key, value in fields.items():
            if generated_identity.get(key) != value:
                raise AssertionError("version/generated identity mismatch: " + key)
        if generated_identity.get("archive") is not (fields["commit"] == ""):
            raise AssertionError("version/generated identity mismatch: archive")
    return fields


class VersionIdentityTests(unittest.TestCase):
    def test_repository_shape_accepts_full_commit(self):
        fields = assert_version_identity(
            "WitcherFG 0.1.0-preview.1 Release commit=" + "a" * 40 + " input=" + "b" * 64 + " channel=preview dirty=false archive=false diagnostic_schema=1\n")
        self.assertEqual(fields["commit"], "a" * 40)
        self.assertEqual(fields["input_digest"], "b" * 64)

    def test_archive_shape_keeps_commit_empty(self):
        fields = assert_version_identity(
            "WitcherFG 0.1.0-preview.1 Release commit= input=" + "b" * 64 + " channel=preview dirty=false archive=true diagnostic_schema=1\n")
        self.assertEqual(fields["commit"], "")
        self.assertEqual(fields["input_digest"], "b" * 64)

    def test_identity_line_accepts_actual_completion_output(self):
        line = "WitcherFG 0.1.0-preview.1 Release commit=" + "a" * 40 + " input=" + "b" * 64 + " channel=preview dirty=false archive=false diagnostic_schema=1"
        output = line + "\nThe read-only operation completed.\nCompleted UTC: 2026-10-10T23:24:33Z elapsed_ms=0\n"
        self.assertEqual(assert_version_identity(output)["commit"], "a" * 40)
        with self.assertRaises(AssertionError):
            assert_version_identity(line + "\n" + line + "\n")

    def test_short_or_malformed_commit_and_digest_are_rejected(self):
        for commit in ("a" * 7, "a" * 39, "a" * 41, "g" * 40):
            with self.subTest(commit=commit), self.assertRaises(AssertionError):
                assert_version_identity("WitcherFG 0.1.0-preview.1 Release commit=" + commit + " input=" + "b" * 64)
        for commit in ("", "a" * 40):
            for digest in ("", "b" * 63, "b" * 65, "g" * 64):
                with self.subTest(commit=commit, digest=digest), self.assertRaises(AssertionError):
                    assert_version_identity("WitcherFG 0.1.0-preview.1 Release commit=" + commit + " input=" + digest)
        with self.assertRaises(AssertionError):
            assert_version_identity("WitcherFG 0.1.0-preview.1 Release commit= input=" + "b" * 64 + " extra")

    def test_actual_generated_repository_and_archive_identity_are_consistent(self):
        from tools.build_identity import make_identity
        metadata = {"version": "0.1.0-preview.1", "channel": "preview", "diagnostic_schema": 1}
        for archive in (False, True):
            generated = make_identity(metadata, {"commit": "a" * 40, "archive": archive, "inputs": {}}, "Release")
            text = "WitcherFG 0.1.0-preview.1 Release commit=" + generated["commit"] + " input=" + generated["input_digest"] + " channel=preview dirty=false archive=" + str(archive).lower() + " diagnostic_schema=1"
            with self.subTest(archive=archive):
                fields = assert_version_identity(text, generated)
                self.assertEqual(fields["commit"], "" if archive else "a" * 40)

    def test_supplied_identity_mismatch_is_rejected(self):
        text = "WitcherFG 0.1.0-preview.1 Release commit=" + "a" * 40 + " input=" + "b" * 64 + " channel=preview dirty=false archive=false diagnostic_schema=1"
        identity = {"version": "0.1.0-preview.1", "build_type": "Release", "commit": "a" * 40,
                    "input_digest": "b" * 64, "archive": False, "dirty": False, "channel": "preview", "diagnostic_schema": 1}
        self.assertEqual(assert_version_identity(text, identity)["commit"], "a" * 40)
        for key, wrong in (("version", "0.2.0"), ("build_type", "Debug"), ("commit", "c" * 40),
                           ("input_digest", "c" * 64), ("archive", True), ("dirty", True), ("channel", "stable"), ("diagnostic_schema", 2)):
            with self.subTest(key=key), self.assertRaises(AssertionError):
                assert_version_identity(text, {**identity, key: wrong})


def compatibility_fixture(scope="diagnostic_probe", status="Verified"):
    return [{"component": "synthetic evidence row", "status": status, "scope": scope,
             "native_release_status": "Pending", "evidence_method": "local measured probe",
             "evidence_date": "2026-10-10", "game_identity": "Xbox package 5.0.4.0",
             "details": "Diagnostic scope only."}]


class MatrixContracts(unittest.TestCase):
    def test_probe_does_not_promote_release(self):
        rows = compatibility_fixture()
        self.assertEqual(validate_matrix(rows), [])
        self.assertEqual(format_matrix_status(rows[0]), "Verified: diagnostic probe")
        self.assertEqual(rows[0]["native_release_status"], "Pending")
        rows[0]["native_release_status"] = "Verified"
        self.assertTrue(validate_matrix(rows))

    def test_verified_requires_measured_evidence(self):
        for key in ("evidence_method", "evidence_date", "game_identity"):
            rows = compatibility_fixture()
            rows[0][key] = ""
            self.assertTrue(validate_matrix(rows), key)
        rows = compatibility_fixture()
        rows[0]["status"] = "Reported"
        rows[0]["evidence_date"] = ""
        self.assertEqual(validate_matrix(rows), [])

    def test_release_requires_exact_artifact_and_build(self):
        rows = compatibility_fixture("native_release")
        rows[0]["native_release_status"] = "Verified"
        self.assertTrue(validate_matrix(rows))
        rows[0].update(artifact_sha256="a" * 64, build_id="b" * 64)
        self.assertEqual(validate_matrix(rows), [])
        self.assertEqual(format_matrix_status(rows[0]), "Verified: native release")

    def test_bad_status_scope_or_schema_is_rejected(self):
        for key, value in (("status", "Tested"), ("scope", "FPS_ratio"),
                           ("native_release_status", "Ready"), ("details", None)):
            rows = compatibility_fixture()
            rows[0][key] = value
            self.assertTrue(validate_matrix(rows), key)


class PublicDocumentation(unittest.TestCase):
    def test_required_public_files_exist(self):
        self.assertEqual([p for p in PUBLIC_FILES if not (ROOT / p).is_file()], [])

    def test_public_links_resolve_without_private_or_placeholder_destinations(self):
        self.assertEqual(validate_public_links(ROOT), [])

    def test_link_checker_rejects_private_missing_and_fake_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for name in PUBLIC_FILES:
                if name.endswith((".md", ".bbcode")):
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text("", encoding="utf-8")
            (root / "README.md").write_text(
                "[private](docs/superpowers/plan.md)\n[missing](absent.md)\n"
                "[release](https://github.com/OWNER/REPO/releases)\n", encoding="utf-8")
            errors = validate_public_links(root)
            self.assertTrue(any("private link" in e for e in errors))
            self.assertTrue(any("missing link target" in e for e in errors))
            self.assertTrue(any("unpublished URL" in e for e in errors))

    def test_matrix_source_and_display_keep_evidence_scope(self):
        path = ROOT / "docs/compatibility.toml"
        self.assertTrue(path.is_file(), "compatibility source missing")
        rows = tomllib.loads(path.read_text(encoding="utf-8"))["compatibility"]
        self.assertEqual(validate_matrix(rows), [])
        display = (ROOT / "docs/compatibility.md").read_text(encoding="utf-8")
        for row in rows:
            self.assertIn(f"| {row['component']} | {format_matrix_status(row)} | {row['native_release_status']} |", display)

    def test_troubleshooting_covers_actual_stable_codes(self):
        path = ROOT / "docs/troubleshooting.md"
        self.assertTrue(path.is_file(), "canonical troubleshooting source missing")
        source = (ROOT / "src/diagnostics.cpp").read_text(encoding="utf-8")
        codes = set(re.findall(r'"(WFG-[EW]-[A-Z_]+)"', source))
        documented = set(re.findall(r"WFG-[EW]-[A-Z_]+", path.read_text(encoding="utf-8")))
        self.assertEqual(codes - documented, set())

    def test_bug_form_requires_reproducible_nonprivate_fields(self):
        path = ROOT / ".github/ISSUE_TEMPLATE/bug_report.yml"
        self.assertTrue(path.is_file(), "bug form missing")
        text = path.read_text(encoding="utf-8")
        blocks = re.split(r"(?m)^  - type:", text)
        required = {"build", "game", "environment", "mode", "mods", "error", "reproduction", "result"}
        found = set()
        for block in blocks:
            match = re.search(r"(?m)^    id: (\w+)$", block)
            if match and re.search(r"(?m)^      required: true$", block):
                found.add(match.group(1))
        self.assertTrue(required <= found, required - found)

    def test_licenses_separate_project_and_complete_dependency_notice(self):
        self.assertTrue((ROOT / "LICENSE").is_file(), "project license missing")
        self.assertTrue((ROOT / "THIRD_PARTY_NOTICES.md").is_file(), "third-party notices missing")
        notice = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        dependencies = tomllib.loads((ROOT / "third_party/dependencies.toml").read_text(encoding="utf-8"))
        for dependency in dependencies["dependency"]:
            license_text = (ROOT / "third_party" / dependency["license_file"]).read_text(encoding="utf-8")
            self.assertIn(license_text.strip(), notice)
            self.assertIn(dependency["version"], notice)
        self.assertIn("MIT License", (ROOT / "LICENSE").read_text(encoding="utf-8"))


class NativeDocumentation(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("WFG_TEST_RUNNER"), "set WFG_TEST_RUNNER for actual native parser")
    def test_example_config_and_missing_file_use_actual_defaults(self):
        runner = pathlib.Path(os.environ["WFG_TEST_RUNNER"])
        config = ROOT / "config/FrameGenFix.toml"
        actual = read_config_snapshot(runner, config)
        with tempfile.TemporaryDirectory() as temporary:
            defaults = read_config_snapshot(runner, pathlib.Path(temporary) / "missing.toml")
        expected = {"schema_version": 1, "logging": {"level": "error", "file_logging": True,
                    "max_file_mib": 2, "retained_files": 5}, "diagnostics": {
                    "runtime_observation": False, "sample_interval_ms": 1000, "observation_seconds": 30}}
        for key, value in expected.items():
            self.assertEqual(actual[key], value)
            self.assertEqual(defaults[key], value)
        self.assertEqual(actual["origins"]["logging.level"], "file")
        self.assertEqual(defaults["origins"]["logging.level"], "default")

    @unittest.skipUnless(os.environ.get("WFG_TEST_RUNNER"), "set WFG_TEST_RUNNER for actual native parser")
    def test_example_config_cannot_add_guard_bypass(self):
        runner = pathlib.Path(os.environ["WFG_TEST_RUNNER"])
        with tempfile.TemporaryDirectory() as temporary:
            config = pathlib.Path(temporary) / "invalid.toml"
            config.write_text("schema_version = 1\nskip_validation = true\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, r"refused \(2\).*CONFIG_INVALID"):
                read_config_snapshot(runner, config)

    @unittest.skipUnless(os.environ.get("WFG_LAUNCHER"), "set WFG_LAUNCHER for actual native help/version")
    def test_documented_options_match_help_and_version_metadata(self):
        binary = os.environ["WFG_LAUNCHER"]
        help_result = subprocess.run([binary, "--help"], capture_output=True, text=True, timeout=20)
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        options = set(re.findall(r"--[a-z][a-z-]*", help_result.stdout))
        config_doc = (ROOT / "docs/configuration.md").read_text(encoding="utf-8")
        self.assertEqual(options, set(re.findall(r"--[a-z][a-z-]*", config_doc)))
        version = subprocess.run([binary, "--version"], capture_output=True, text=True, timeout=20)
        self.assertEqual(version.returncode, 0, version.stderr)
        metadata = tomllib.loads((ROOT / "release/project.toml").read_text(encoding="utf-8"))
        self.assertIn("WitcherFG " + metadata["version"] + " ", version.stdout)
        identity_path = os.environ.get("WFG_BUILD_IDENTITY")
        generated = json.loads(pathlib.Path(identity_path).read_text(encoding="utf-8-sig")) if identity_path else None
        assert_version_identity(version.stdout, generated)


if __name__ == "__main__":
    unittest.main()
