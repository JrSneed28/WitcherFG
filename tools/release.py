"""Offline rendering and fail-closed packaging of a clean Windows candidate.

Only the standard library is used. Acceptance is external to the immutable ZIP.
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import platform
import re
import subprocess
import sys
import tomllib
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from tools import build_identity

IDENTITY_KEYS = ("version", "channel", "build_type", "commit", "dirty", "archive", "diagnostic_schema", "input_digest")
DOCUMENT_NAMES = {"README.txt", "FrameGenFix.toml", "CHANGELOG.txt", "THIRD_PARTY_NOTICES.txt",
                  "TROUBLESHOOTING.txt", "UNINSTALL.txt", "LICENSE", "licenses/tomlplusplus-LICENSE.txt"}
PAYLOAD_NAMES = DOCUMENT_NAMES | {"WitcherFG.exe", "VERSION.txt"}
ARCHIVE_NAMES = PAYLOAD_NAMES | {"SHA256SUMS"}
ACCEPTANCE_CASES = {"fresh_install", "upgrade_config", "gameplay_fg", "guard_refusal", "uninstall_restore"}
MAX_ARCHIVE = 32 * 1024 * 1024
MAX_ENTRY = 16 * 1024 * 1024
VERSION = r"0|[1-9][0-9]*"
SEMVER = rf"(?:{VERSION})\.(?:{VERSION})\.(?:{VERSION})(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _read(path: pathlib.Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ValueError("missing or symlink input: " + path.name)
    data = path.read_bytes()
    if len(data) > MAX_ENTRY:
        raise ValueError("input exceeds payload bound: " + path.name)
    return data


def load_metadata(path: pathlib.Path) -> dict:
    return _parse_metadata(_read(path))


def _parse_metadata(data: bytes) -> dict:
    metadata = tomllib.loads(data.decode("utf-8"))
    if not re.fullmatch(SEMVER, metadata.get("version", "")):
        raise ValueError("invalid canonical version")
    if metadata.get("channel") not in {"preview", "stable"}:
        raise ValueError("invalid canonical channel")
    if type(metadata.get("diagnostic_schema")) is not int or metadata["diagnostic_schema"] != 1:
        raise ValueError("unsupported diagnostic schema")
    for key in ("support_url", "release_url"):
        value = metadata.get(key)
        if not isinstance(value, str) or (value and not re.fullmatch(r"https://[A-Za-z0-9.-]+/[^\s<>\[\]]*", value)):
            raise ValueError("invalid publication URL")
        if value and re.search(r"example\.|OWNER|REPO|placeholder", value, re.I):
            raise ValueError("unpublished placeholder URL")
    return metadata


def _validate_identity(identity: dict, candidate=True) -> None:
    if not isinstance(identity, dict):
        raise ValueError("identity must be an object")
    if not re.fullmatch(SEMVER, identity.get("version", "")):
        raise ValueError("malformed identity version")
    if identity.get("channel") not in {"preview", "stable"} or identity.get("build_type") not in {"Release", "Debug"}:
        raise ValueError("malformed channel/build type")
    for key in ("dirty", "archive"):
        if type(identity.get(key)) is not bool:
            raise ValueError("malformed identity boolean: " + key)
    if type(identity.get("diagnostic_schema")) is not int or identity["diagnostic_schema"] != 1:
        raise ValueError("malformed diagnostic schema")
    commit = identity.get("commit")
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}|", commit) or identity["archive"] != (commit == ""):
        raise ValueError("malformed source provenance")
    if not re.fullmatch(r"[0-9a-f]{64}", identity.get("input_digest", "")):
        raise ValueError("malformed build-input digest")
    if candidate and (identity["dirty"] or identity["archive"] or identity["build_type"] != "Release"):
        raise ValueError("candidate requires clean committed Release identity")


def parse_version(text: str) -> dict:
    lines = [line for line in text.splitlines() if line.startswith("WitcherFG ")]
    if len(lines) != 1:
        raise ValueError("expected one executable version identity line")
    match = re.fullmatch(
        rf"WitcherFG (?P<version>{SEMVER}) (?P<build_type>Release|Debug) "
        r"commit=(?P<commit>(?:[0-9a-f]{40})?) input=(?P<input_digest>[0-9a-f]{64}) "
        r"channel=(?P<channel>preview|stable) dirty=(?P<dirty>true|false) "
        r"archive=(?P<archive>true|false) diagnostic_schema=(?P<diagnostic_schema>[1-9][0-9]*)", lines[0])
    if not match:
        raise ValueError("malformed executable version contract")
    fields = match.groupdict()
    fields["dirty"] = fields["dirty"] == "true"
    fields["archive"] = fields["archive"] == "true"
    fields["diagnostic_schema"] = int(fields["diagnostic_schema"])
    _validate_identity(fields, candidate=False)
    return fields


def check_identity(actual: dict, generated: dict, metadata: dict, digest: str) -> None:
    _validate_identity(actual)
    _validate_identity(generated)
    if any(actual.get(key) != generated.get(key) for key in IDENTITY_KEYS):
        raise ValueError("executable/generated identity mismatch")
    if any(actual[key] != metadata[key] for key in build_identity.IDENTITY_FIELDS):
        raise ValueError("executable/canonical metadata mismatch")
    if actual["input_digest"] != digest:
        raise ValueError("executable/current code or build inputs mismatch")


def _escape(text: str, target: str) -> str:
    if target == "bbcode":
        return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("[", "&#91;").replace("]", "&#93;")
    return text


def _inline(text: str, target: str) -> str:
    # Recognize the supported tokens first, then escape every literal segment.
    pattern = re.compile(r"`([^`\n]+)`|\*\*([^*\n]+)\*\*|\[([^\[\]\n]+)\]\(([^()\s]+)\)")
    output = []
    position = 0
    for match in pattern.finditer(text):
        output.append(_escape(text[position:match.start()], target))
        if match.group(1) is not None:
            output.append(_escape(match.group(1), target))
        elif match.group(2) is not None:
            content = _escape(match.group(2), target)
            output.append("[b]" + content + "[/b]" if target == "bbcode" else content)
        else:
            label, url = match.group(3, 4)
            if re.search(r"[<>\"\[\]]", url) or re.match(r"(?:javascript|data|file):", url, re.I):
                raise ValueError("unsupported link destination")
            if target == "bbcode" and url.startswith("https://"):
                output.append("[url=" + _escape(url, target) + "]" + _escape(label, target) + "[/url]")
            else:
                output.append(_escape(label + " (" + url + ")", target))
        position = match.end()
    output.append(_escape(text[position:], target))
    return "".join(output)


def render_markdown(source: str, target: str) -> str:
    """Headings, flat lists, paragraphs, inline code/bold/links, fences, tables.

    A fenced block may be indented up to three spaces (canonical list examples).
    HTML, images, nested lists, blockquotes and unsupported inline constructs fail.
    """
    if target not in {"text", "bbcode"}:
        raise ValueError("unknown renderer target")
    lines = source.replace("\r\n", "\n").splitlines()
    result = []
    fence = False
    for line in lines:
        stripped = line.strip()
        if re.fullmatch(r" {0,3}```[A-Za-z0-9_-]*", line):
            fence = not fence
            if target == "bbcode":
                result.append("[code]" if fence else "[/code]")
            continue
        if fence:
            result.append(_escape(line.lstrip(" ") if line.startswith("   ") else line, target))
            continue
        without_emails = re.sub(r"<[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}>", "", line)
        if re.search(r"<[^>]+>|!\[|^\s*>|^\s{2,}(?:[-*+] |\d+\. )", without_emails):
            raise ValueError("unsupported HTML/image/nesting/blockquote")
        if re.search(r"(?<!\*)\*(?!\*)|~~|(?<!\\)_", re.sub(r"`[^`]+`", "", line)):
            # Underscores are ordinary word characters; only explicit emphasis is unsupported.
            if re.search(r"(?:^|\s)_[^\s].*_|~~|(?<!\*)\*(?!\*)", re.sub(r"`[^`]+`", "", line)):
                raise ValueError("unsupported inline formatting")
        heading = re.fullmatch(r"#{1,6} (.+)", line)
        if heading:
            content = _inline(heading.group(1), target)
            result.append("[b]" + content + "[/b]" if target == "bbcode" else content)
        elif re.fullmatch(r"\|?\s*:?-{3,}:?\s*(?:\|\s*:?-{3,}:?\s*)+\|?", stripped):
            continue
        elif stripped.startswith("|") and stripped.endswith("|"):
            result.append(" | ".join(_inline(c.strip(), target) for c in stripped[1:-1].split("|")))
        elif re.fullmatch(r"-{3,}", stripped):
            result.append("")
        else:
            result.append(_inline(line, target))
    if fence:
        raise ValueError("unterminated Markdown fence")
    return "\n".join(result).strip() + "\n"


def _txt(text: str) -> bytes:
    return (text.replace("\r\n", "\n").rstrip() + "\n").replace("\n", "\r\n").encode("utf-8")


def _matrix_rows(root: pathlib.Path) -> list[tuple[dict, str]]:
    # Share the public evidence contract; do not invent a second meaning of Verified.
    import importlib.util
    spec = importlib.util.spec_from_file_location("wfg_documentation_contract", ROOT / "tests/documentation_tests.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    rows = tomllib.loads(_read(root / "docs/compatibility.toml").decode("utf-8"))["compatibility"]
    errors = module.validate_matrix(rows)
    if errors:
        raise ValueError("invalid compatibility evidence: " + "; ".join(errors))
    return [(row, module.format_matrix_status(row)) for row in rows]


def _matrix_table(root: pathlib.Path) -> str:
    result = ["| Component | Evidence status | Native release status | Details |", "| --- | --- | --- | --- |"]
    for row, status in _matrix_rows(root):
        cells = [row["component"], status, row["native_release_status"], row["details"]]
        if any("|" in value or "\n" in value for value in cells):
            raise ValueError("unsupported matrix cell formatting")
        result.append("| " + " | ".join(cells) + " |")
    return "\n".join(result)


def render_documents(metadata: dict, docs_root: pathlib.Path) -> dict[str, bytes]:
    if metadata.get("channel") != "preview":
        raise ValueError("packaging only creates preview candidates; stable requires external live release decision")
    root = docs_root.parent
    config = _read(root / "config/FrameGenFix.toml")
    expected = {"schema_version": 1, "logging": {"level": "error", "file_logging": True, "max_file_mib": 2, "retained_files": 5},
                "diagnostics": {"runtime_observation": False, "sample_interval_ms": 1000, "observation_seconds": 30}}
    if tomllib.loads(config.decode("utf-8")) != expected:
        raise ValueError("canonical example differs from native default contract")
    changelog = _read(root / "CHANGELOG.md").decode("utf-8")
    if not re.search(r"(?m)^## " + re.escape(metadata["version"]) + r"(?: |$)", changelog):
        raise ValueError("changelog lacks canonical version")
    summary = _read(docs_root / "release-summary.md").decode("utf-8")
    if metadata["version"] not in summary or "unpublished" not in summary:
        raise ValueError("release summary metadata drift")
    notice = _read(root / "THIRD_PARTY_NOTICES.md").decode("utf-8")
    dependencies = tomllib.loads(_read(root / "third_party/dependencies.toml").decode("utf-8"))["dependency"]
    if len(dependencies) != 1 or dependencies[0]["name"] != "toml++":
        raise ValueError("unreviewed dependency inventory")
    dependency = dependencies[0]
    license_data = _read(root / "third_party" / dependency["license_file"])
    if sha256(license_data) != dependency["license_sha256"] or license_data.decode("utf-8").strip() not in notice:
        raise ValueError("missing or mismatched required dependency license")
    project_license = _read(root / "LICENSE")
    if b"MIT License" not in project_license:
        raise ValueError("missing project license")
    _matrix_rows(root)
    readme = summary + "\n" + _read(docs_root / "installation.md").decode("utf-8")
    readme += "\nRemoval: follow UNINSTALL.txt. Recovery and support: follow TROUBLESHOOTING.txt.\nA source ZIP is developer input, not this portable player installer.\n"
    return {"README.txt": _txt(render_markdown(readme, "text")),
            "FrameGenFix.toml": config.replace(b"\r\n", b"\n"),
            "CHANGELOG.txt": _txt(render_markdown(changelog, "text")),
            "THIRD_PARTY_NOTICES.txt": _txt(render_markdown(notice, "text")),
            "TROUBLESHOOTING.txt": _txt(render_markdown(_read(docs_root / "troubleshooting.md").decode("utf-8"), "text")),
            "UNINSTALL.txt": _txt(render_markdown(_read(docs_root / "uninstall.md").decode("utf-8"), "text")),
            "LICENSE": project_license.replace(b"\r\n", b"\n"),
            "licenses/tomlplusplus-LICENSE.txt": _txt(license_data.decode("utf-8"))}


def _managed(text: str, name: str, value: str, initial: str) -> str:
    begin, end = f"<!-- BEGIN WFG {name} -->", f"<!-- END WFG {name} -->"
    block = begin + "\n" + value.strip() + "\n" + end
    if begin in text or end in text:
        if text.count(begin) != 1 or text.count(end) != 1:
            raise ValueError("malformed managed document block")
        return re.sub(re.escape(begin) + r"[\s\S]*?" + re.escape(end), lambda _: block, text)
    if initial not in text:
        raise ValueError("managed document seed missing")
    return text.replace(initial, block, 1)


def render_tracked(metadata: dict, root: pathlib.Path, check: bool) -> list[str]:
    render_documents(metadata, root / "docs")
    compatibility = _read(root / "docs/compatibility.md").decode("utf-8")
    table = re.search(r"(?m)^\| Component[^\n]*\n(?:\|[^\n]*\n)+", compatibility)
    if not table:
        raise ValueError("compatibility table seed missing")
    compatibility = _managed(compatibility, "COMPATIBILITY", _matrix_table(root), table.group().rstrip())
    summary = _read(root / "docs/release-summary.md").decode("utf-8")
    bbcode = render_markdown(summary + "\n## Compatibility and evidence scope\n\n" + _matrix_table(root), "bbcode")
    readme = _read(root / "README.md").decode("utf-8")
    seed = re.search(r"(?m)^\*\*Status:.*(?:\n(?!\n).*)*", readme)
    if not seed:
        raise ValueError("README status seed missing")
    status = f"**Status: unpublished {metadata['channel']}, {metadata['version']}.** A candidate can be packaged locally. Exact-candidate gameplay/install/upgrade/uninstall acceptance is Pending. No stable release is available."
    status += "\nRelease: " + (metadata["release_url"] or "unpublished") + ". Support: " + (metadata["support_url"] or "unpublished") + "."
    readme = _managed(readme, "STATUS", status, seed.group())
    outputs = {"README.md": readme, "docs/compatibility.md": compatibility, "docs/NEXUS_DESCRIPTION.bbcode": bbcode}
    changed = []
    for name, value in outputs.items():
        data = (value.rstrip() + "\n").encode("utf-8")
        if (root / name).read_bytes() != data:
            changed.append(name)
            if not check:
                (root / name).write_bytes(data)
    return changed


def make_install_archive(identity: dict, binary: pathlib.Path, documents: dict[str, bytes], output: pathlib.Path) -> pathlib.Path:
    _validate_identity(identity)
    if identity["channel"] != "preview":
        raise ValueError("no stable archive without actual live acceptance")
    if set(documents) != DOCUMENT_NAMES:
        raise ValueError("installer document allowlist mismatch")
    epoch = identity.get("commit_epoch")
    if type(epoch) is not int:
        raise ValueError("recorded source commit timestamp required")
    stamp = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    if not 1980 <= stamp.year <= 2107:
        raise ValueError("source timestamp outside ZIP range")
    date = (stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second // 2 * 2)
    version = "WitcherFG " + identity["version"] + "\nCandidate: live acceptance Pending; unpublished\n"
    version += json.dumps({key: identity[key] for key in IDENTITY_KEYS}, sort_keys=True, separators=(",", ":")) + "\n"
    payload = {**documents, "WitcherFG.exe": _read(binary), "VERSION.txt": _txt(version)}
    if any(not isinstance(data, bytes) or len(data) > MAX_ENTRY for data in payload.values()) or sum(map(len, payload.values())) > MAX_ARCHIVE:
        raise ValueError("installer payload bound exceeded")
    for name, data in payload.items():
        if name.endswith(".txt"):
            text = data.decode("utf-8")
            if text.startswith("\ufeff") or "\n" in text.replace("\r\n", "") or not text.endswith("\r\n"):
                raise ValueError("TXT encoding/endings mismatch")
    payload["SHA256SUMS"] = ("".join(sha256(payload[name]) + "  " + name + "\n" for name in sorted(payload))).encode("ascii")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED, allowZip64=False) as archive:
        for name in sorted(payload):
            entry = zipfile.ZipInfo(name, date)
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, payload[name])
    return output


def _validate_acceptance(validation: dict, identity: dict, archive_hash: str, binary_hash: str) -> list[str]:
    errors = []
    if not isinstance(validation, dict) or validation.get("schema") != 1:
        return ["invalid external validation schema"]
    expected = {"version": identity["version"], "artifact_sha256": archive_hash, "binary_sha256": binary_hash,
                "source_commit": identity["commit"], "input_digest": identity["input_digest"], "build_id": identity["input_digest"]}
    for key, value in expected.items():
        if validation.get(key) != value:
            errors.append("validation artifact mismatch: " + key)
    tag = validation.get("tag")
    if not isinstance(tag, str) or (tag and tag != "v" + identity["version"]):
        errors.append("validation tag/version mismatch")
    if validation.get("publication_ready") is not True:
        errors.append("validation publication readiness is not established")
    environment = validation.get("environment")
    if not isinstance(environment, dict) or any(not isinstance(environment.get(k), str) or not environment[k].strip()
                                              for k in ("windows", "gpu", "driver", "game_package", "method")):
        errors.append("validation lacks environment/method")
    limits = validation.get("known_limits")
    if not isinstance(limits, list) or any(not isinstance(item, str) for item in limits):
        errors.append("validation lacks explicit known limits")
    cases = validation.get("cases")
    if not isinstance(cases, dict) or set(cases) != ACCEPTANCE_CASES:
        errors.append("validation lacks required live cases")
    else:
        for name, case in cases.items():
            if not isinstance(case, dict) or case.get("status") != "Passed" or not isinstance(case.get("evidence"), str) or not case["evidence"].strip():
                errors.append("validation case lacks passed evidence: " + name)
    for channel in ("github_sha256", "nexus_sha256"):
        if channel in validation and validation[channel] != archive_hash:
            errors.append("publication channel ZIP differs: " + channel)
    return errors


def verify_release(archive: pathlib.Path, identity: dict, validation: dict | None) -> list[str]:
    """Check ZIP/record consistency; the public CLI also proves any claimed Git tag."""
    errors = []
    try:
        _validate_identity(identity)
        raw = _read(archive)
        if len(raw) > MAX_ARCHIVE:
            return ["archive size exceeds bound"]
        with zipfile.ZipFile(archive) as package:
            entries = package.infolist()
            names = [entry.filename for entry in entries]
            if len(names) != len(set(names)) or set(names) != ARCHIVE_NAMES:
                return ["archive names duplicate, unsafe, missing or outside allowlist"]
            if any(e.file_size > MAX_ENTRY or e.compress_type != zipfile.ZIP_STORED or e.flag_bits & 1 or
                   (e.external_attr >> 16) & 0o170000 != 0o100000 for e in entries):
                return ["unsupported archive entry type, bounds or compression"]
            if sum(e.file_size for e in entries) > MAX_ARCHIVE or package.testzip() is not None:
                return ["archive payload bound or CRC failure"]
            payload = {entry.filename: package.read(entry) for entry in entries}
        expected_sums = "".join(sha256(payload[name]) + "  " + name + "\n" for name in sorted(PAYLOAD_NAMES)).encode("ascii")
        if payload["SHA256SUMS"] != expected_sums:
            errors.append("payload SHA256 manifest mismatch")
        if payload["LICENSE"] != _read(ROOT / "LICENSE").replace(b"\r\n", b"\n"):
            errors.append("project license mismatch")
        expected_config = _read(ROOT / "config/FrameGenFix.toml").replace(b"\r\n", b"\n")
        if payload["FrameGenFix.toml"] != expected_config:
            errors.append("canonical default configuration mismatch")
        dependency_license = _read(ROOT / "third_party/tomlplusplus/LICENSE").decode("utf-8")
        if payload["licenses/tomlplusplus-LICENSE.txt"] != _txt(dependency_license):
            errors.append("required dependency license mismatch")
        if dependency_license.strip() not in payload["THIRD_PARTY_NOTICES.txt"].decode("utf-8").replace("\r\n", "\n"):
            errors.append("required full dependency notice missing")
        readme = payload["README.txt"].decode("utf-8")
        if identity["version"] not in readme or "source ZIP" not in readme or "Pending" not in readme:
            errors.append("candidate/version/source ZIP warning missing")
        version_lines = payload["VERSION.txt"].decode("utf-8").splitlines()
        actual = json.loads(version_lines[-1])
        if any(actual.get(key) != identity[key] for key in IDENTITY_KEYS):
            errors.append("installer build identity mismatch")
        if version_lines[0] != "WitcherFG " + identity["version"] or version_lines[1] != "Candidate: live acceptance Pending; unpublished":
            errors.append("installer candidate/version label mismatch")
        for name, data in payload.items():
            if name.endswith(".txt"):
                text = data.decode("utf-8")
                if text.startswith("\ufeff") or "\n" in text.replace("\r\n", "") or not text.endswith("\r\n"):
                    errors.append("TXT encoding/endings mismatch: " + name)
        if validation is not None:
            errors.extend(_validate_acceptance(validation, identity, sha256(raw), sha256(payload["WitcherFG.exe"])))
    except (OSError, ValueError, KeyError, IndexError, TypeError, zipfile.BadZipFile, RuntimeError) as error:
        errors.append("archive verification failed: " + str(error))
    return errors


def _run(arguments: list[str], cwd: pathlib.Path = ROOT) -> str:
    result = subprocess.run(arguments, cwd=cwd, capture_output=True, text=True, timeout=20)
    if result.returncode:
        raise ValueError("bounded command failed: " + pathlib.Path(arguments[0]).name)
    if len(result.stdout.encode("utf-8")) > 1024 * 1024:
        raise ValueError("command output exceeds bound")
    return result.stdout


def _git(*arguments: str) -> str:
    return _run(["git", *arguments]).strip()


def _current_digest(metadata: dict) -> str:
    return build_identity.make_identity(metadata, {"commit": "", "archive": True,
              "inputs": build_identity._collect_inputs(ROOT)}, "Release")["input_digest"]


def _verify_acceptance_tag(tag: str, identity: dict) -> None:
    if not tag:
        return
    if tag != "v" + identity["version"]:
        raise ValueError("acceptance tag/version mismatch")
    try:
        # Use an exact tag ref, never a branch/shorthand; peel annotated tags to a commit.
        commit = _git("rev-parse", "--verify", "refs/tags/" + tag + "^{commit}")
        if not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise ValueError("invalid tagged commit")
        inputs = {}
        metadata_bytes = None
        for entry in _git("ls-tree", "-r", "-z", "--full-tree", commit).split("\0"):
            if not entry:
                continue
            header, name = entry.split("\t", 1)
            mode, kind, object_id = header.split()
            if name != "release/project.toml" and not build_identity._is_build_input(name):
                continue
            if kind != "blob" or mode not in {"100644", "100755"}:
                raise ValueError("nonregular tagged build input")
            if int(_git("cat-file", "-s", object_id)) > MAX_ENTRY:
                raise ValueError("tagged input exceeds bound")
            result = subprocess.run(["git", "cat-file", "blob", object_id], cwd=ROOT,
                                    capture_output=True, timeout=20)
            if result.returncode or len(result.stdout) > MAX_ENTRY:
                raise ValueError("unavailable tagged input")
            if name == "release/project.toml":
                metadata_bytes = result.stdout
            else:
                inputs[name] = result.stdout
        if metadata_bytes is None:
            raise ValueError("missing tagged canonical metadata")
        metadata = _parse_metadata(metadata_bytes)
        tagged = build_identity.make_identity(metadata,
            {"commit": commit, "dirty": False, "archive": False, "inputs": inputs}, identity["build_type"])
        if any(tagged[key] != identity[key] for key in build_identity.IDENTITY_FIELDS) or \
                tagged["input_digest"] != identity["input_digest"]:
            raise ValueError("tagged metadata or build inputs differ from tested binary")
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        raise ValueError("acceptance tag provenance failed: " + str(error)) from error


def record_toolchain(build_root: pathlib.Path) -> dict:
    cache = _read(build_root / "CMakeCache.txt").decode("utf-8")
    record = {key: value for key, value in re.findall(
        r"(?m)^(CMAKE_GENERATOR|CMAKE_GENERATOR_PLATFORM):[^=]+=(.*)$", cache)}
    compilers = list((build_root / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if len(compilers) != 1:
        raise ValueError("ambiguous or missing recorded compiler")
    compiler = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([0-9.]+)"\)', _read(compilers[0]).decode("utf-8"))
    sdks = set()
    for project in build_root.glob("*.vcxproj"):
        sdks.update(re.findall(r"<WindowsTargetPlatformVersion>([0-9.]+)</WindowsTargetPlatformVersion>", _read(project).decode("utf-8")))
    if not compiler or len(sdks) != 1:
        raise ValueError("missing or ambiguous MSVC/SDK evidence")
    record.update(msvc=compiler.group(1), windows_sdk=sdks.pop(), cmake=compilers[0].parent.name,
                  python=sys.version.split()[0], windows=platform.version(), architecture=platform.machine())
    return record


def package_candidate(build_dir: pathlib.Path, out_dir: pathlib.Path) -> pathlib.Path:
    metadata = load_metadata(ROOT / "release/project.toml")
    if _git("status", "--porcelain", "--untracked-files=all"):
        raise ValueError("candidate packaging requires a clean source checkout")
    if render_tracked(metadata, ROOT, check=True):
        raise ValueError("generated public summaries are stale")
    binary = build_dir / "WitcherFG.exe"
    generated_path = build_dir.parent / "generated" / build_dir.name / "wfg/build_info.json"
    generated = json.loads(_read(generated_path).decode("utf-8"))
    actual = parse_version(_run([str(binary.resolve()), "--version"]))
    check_identity(actual, generated, metadata, _current_digest(metadata))
    # Documentation-only commits may follow the tested binary; code must still match.
    _git("cat-file", "-e", actual["commit"] + "^{commit}")
    head = _git("rev-parse", "HEAD")
    epoch = int(_git("show", "-s", "--format=%ct", actual["commit"]))
    tags = _git("tag", "--points-at", "HEAD").splitlines()
    if tags and any(tag != "v" + metadata["version"] for tag in tags):
        raise ValueError("actual source tag/version mismatch")
    identity = {**actual, "commit_epoch": epoch}
    archive = out_dir / f"WitcherFG-{metadata['version']}-windows-x64.zip"
    documents = render_documents(metadata, ROOT / "docs")
    make_install_archive(identity, binary, documents, archive)
    errors = verify_release(archive, actual, None)
    if errors:
        raise ValueError("; ".join(errors))
    digest = sha256(archive.read_bytes())
    archive.with_suffix(".zip.sha256").write_text(digest + "  " + archive.name + "\n", encoding="ascii", newline="\n")
    manifest = {"schema": 1, "status": "candidate", "live_acceptance": "Pending", "publication_ready": False,
                "archive": archive.name, "artifact_sha256": digest, "binary_sha256": sha256(_read(binary)),
                "build": actual, "build_id": actual["input_digest"], "binary_source_commit": actual["commit"],
                "documentation_commit": head, "commit_epoch": epoch, "actual_tags": tags,
                "payload_sha256": {name: sha256(data) for name, data in {**documents, "WitcherFG.exe": _read(binary)}.items()}}
    # Record actual generated compiler/SDK values without personal installation paths.
    manifest["toolchain"] = record_toolchain(build_dir.parent)
    archive.with_suffix(".manifest.json").write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8", newline="\n")
    return archive


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    render = commands.add_parser("render")
    render.add_argument("--check", action="store_true")
    package = commands.add_parser("package")
    package.add_argument("--build-dir", type=pathlib.Path, required=True)
    package.add_argument("--out-dir", type=pathlib.Path, default=pathlib.Path("dist"))
    verify = commands.add_parser("verify")
    verify.add_argument("--archive", type=pathlib.Path, required=True)
    verify.add_argument("--validation", type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "render":
            changed = render_tracked(load_metadata(ROOT / "release/project.toml"), ROOT, args.check)
            if args.check and changed:
                raise ValueError("rendered files stale: " + ", ".join(changed))
            print("Rendered documents checked" if args.check else "Rendered documents updated")
        elif args.command == "package":
            print(package_candidate(args.build_dir.resolve(), args.out_dir.resolve()).name)
        else:
            manifest = json.loads(_read(args.archive.with_suffix(".manifest.json")).decode("utf-8"))
            metadata = load_metadata(ROOT / "release/project.toml")
            identity = manifest["build"]
            _validate_identity(identity)
            if any(identity[key] != metadata[key] for key in build_identity.IDENTITY_FIELDS) or identity["input_digest"] != _current_digest(metadata):
                raise ValueError("archive/current code or canonical metadata mismatch")
            validation_path = args.validation or ROOT / "release/validation.json"
            validation = json.loads(_read(validation_path).decode("utf-8")) if validation_path.exists() else None
            errors = verify_release(args.archive, identity, validation)
            if sha256(_read(args.archive)) != manifest["artifact_sha256"]:
                errors.append("external candidate archive checksum mismatch")
            with zipfile.ZipFile(args.archive) as archive:
                if sha256(archive.read("WitcherFG.exe")) != manifest["binary_sha256"]:
                    errors.append("external candidate binary checksum mismatch")
            if errors:
                raise ValueError("; ".join(errors))
            if validation is not None:
                _verify_acceptance_tag(validation["tag"], identity)
            print("Archive verified: " + ("external live acceptance matches" if validation else "candidate; live acceptance Pending"))
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError, zipfile.BadZipFile) as error:
        print("Release refused: " + str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
