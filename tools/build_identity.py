import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tomllib


INPUT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".py", ".cmake"}
INPUT_ROOTS = {"include", "src", "tests", "third_party", "tools"}
ROOT_INPUTS = {"CMakeLists.txt", "CMakePresets.json"}
VENDOR_INPUTS = {"third_party/dependencies.toml", "third_party/tomlplusplus/LICENSE"}
IDENTITY_FIELDS = ("version", "channel", "diagnostic_schema")


def _input_bytes(value):
    if isinstance(value, bytes):
        return value
    if isinstance(value, str):
        return value.encode("utf-8")
    raise TypeError("build input values must be bytes or strings")


def _is_build_input(name):
    normalized = name.replace("\\", "/")
    if normalized in ROOT_INPUTS or normalized in VENDOR_INPUTS:
        return True
    parts = normalized.split("/")
    return len(parts) > 1 and parts[0] in INPUT_ROOTS and pathlib.PurePosixPath(normalized).suffix.lower() in INPUT_SUFFIXES


def _digest(metadata, inputs, build_type):
    digest = hashlib.sha256()
    selected_metadata = {key: metadata[key] for key in IDENTITY_FIELDS}
    digest.update(json.dumps(selected_metadata, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    digest.update(str(build_type).encode("utf-8"))
    digest.update(b"\0")
    for name in sorted(name for name in inputs if _is_build_input(name)):
        payload = _input_bytes(inputs[name])
        digest.update(name.replace("\\", "/").encode("utf-8"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(payload).digest())
    return digest.hexdigest()


def make_identity(metadata: dict, git: dict, build_type: str) -> dict:
    for field in IDENTITY_FIELDS:
        if field not in metadata:
            raise ValueError(f"metadata is missing {field}")
    commit = git.get("commit", "")
    if commit and (len(commit) != 40 or any(character not in "0123456789abcdefABCDEF" for character in commit)):
        raise ValueError("git commit must be a full 40 character object ID")
    inputs = git.get("inputs", {})
    if not isinstance(inputs, dict):
        raise TypeError("git inputs must be a mapping")
    archive = bool(git.get("archive", not commit))
    if archive:
        commit = ""
    elif not commit:
        raise ValueError("non-archive builds require a full source commit")
    return {
        "version": str(metadata["version"]),
        "channel": str(metadata["channel"]),
        "build_type": str(build_type),
        "commit": commit.lower(),
        "dirty": bool(git.get("dirty", False)),
        "archive": archive,
        "diagnostic_schema": int(metadata["diagnostic_schema"]),
        "input_digest": _digest(metadata, inputs, build_type),
    }


def _cpp_string(value):
    return json.dumps(str(value), ensure_ascii=True)


def write_header(identity: dict, output: pathlib.Path) -> None:
    fields = ("version", "channel", "build_type", "commit", "input_digest")
    lines = [
        "#pragma once",
        "",
        "#include <string_view>",
        "",
        "namespace wfg {",
        "struct BuildIdentity {",
        "  std::string_view version;",
        "  std::string_view channel;",
        "  std::string_view build_type;",
        "  std::string_view commit;",
        "  bool dirty;",
        "  bool archive;",
        "  unsigned diagnostic_schema;",
        "  std::string_view input_digest;",
        "};",
        "inline constexpr BuildIdentity kEmbeddedBuildIdentity{",
        f"    {_cpp_string(identity['version'])}, {_cpp_string(identity['channel'])}, {_cpp_string(identity['build_type'])},",
        f"    {_cpp_string(identity['commit'])}, {'true' if identity['dirty'] else 'false'}, {'true' if identity['archive'] else 'false'},",
        f"    {int(identity['diagnostic_schema'])}U, {_cpp_string(identity['input_digest'])}}};",
        "}",
        "",
    ]
    output = pathlib.Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    output.with_suffix(".json").write_text(json.dumps(identity, sort_keys=True, indent=2) + "\n", encoding="utf-8", newline="\n")


def _collect_inputs(root):
    inputs = {}
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        top = relative.split("/", 1)[0]
        if top in INPUT_ROOTS and path.suffix.lower() in INPUT_SUFFIXES:
            inputs[relative] = path.read_bytes()
        elif relative in ROOT_INPUTS:
            inputs[relative] = path.read_bytes()
        elif relative in VENDOR_INPUTS:
            inputs[relative] = path.read_bytes()
    return inputs


def _git_state(root):
    try:
        commit = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
        status = subprocess.run(["git", "-C", str(root), "status", "--porcelain", "--untracked-files=all"], check=True, capture_output=True, text=True).stdout
        return {"commit": commit, "dirty": bool(status), "archive": False}
    except (OSError, subprocess.CalledProcessError):
        return {"commit": "", "dirty": False, "archive": True}


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--metadata", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--build-type", required=True)
    options = parser.parse_args(argv)
    root = options.root.resolve()
    metadata = tomllib.loads(options.metadata.read_text(encoding="utf-8"))
    git = _git_state(root)
    git["inputs"] = _collect_inputs(root)
    identity = make_identity(metadata, git, options.build_type)
    write_header(identity, options.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
