# Release status and acceptance

`release/project.toml` is the current canonical version/channel/schema metadata. The version is `0.1.0-preview.1`, channel `preview`, diagnostic schema 1. The release URL remains empty: **unpublished**. The support URL points to [GitHub Issues](https://github.com/JrSneed28/WitcherFG/issues), and [private security reporting](../SECURITY.md) is enabled. No stable release is available.

## Source ZIP and player ZIP

A source ZIP is developer input. Build it using [building](building.md); it is not a portable player package. Native development binaries and synthetic fixture results are also not a player release.

The standard-library release tool creates an unpublished preview candidate. Run from a clean committed Windows checkout after the native Release build:

```powershell
python tools/release.py render --check
python -m unittest discover -s tests -p release_tests.py -v
python tools/release.py package --build-dir build/native/Release --out-dir dist
python tools/release.py verify --archive dist/WitcherFG-0.1.0-preview.1-windows-x64.zip --validation dist/validation.json
```

For the tracked VS17 preset, substitute `build/windows-x64/Release` for the build directory. The draft verification command selects `dist/validation.json`; when that staging record is absent, verification establishes candidate integrity and reports live acceptance Pending. The repository's historical `release/validation.json` describes a different artifact and is not draft acceptance. Before public binary publication, supply an exact-artifact record explicitly with `--validation release/validation.json`; every required acceptance case must pass. `python tools/release.py render` explicitly updates the managed README status, compatibility table and BBCode; commit those source changes before building a candidate. `render --check` reports drift without writing files. Dirty, source-archive, Debug, malformed, stale or conflicting identities refuse packaging. The executable's actual `--version`, generated identity, canonical metadata and recomputed code/build inputs must agree.

The ZIP contains only `WitcherFG.exe`, `README.txt`, `VERSION.txt`, `FrameGenFix.toml`, `CHANGELOG.txt`, `THIRD_PARTY_NOTICES.txt`, `TROUBLESHOOTING.txt`, `UNINSTALL.txt`, `LICENSE`, `licenses/tomlplusplus-LICENSE.txt` and `SHA256SUMS`. TXT files use UTF-8 without a BOM and CRLF. Sorted ZIP-STORE entries use fixed attributes and the binary source commit's UTC timestamp, rounded down to ZIP's two-second precision. Identical package inputs produce identical bytes; recompilation across different toolchains is not promised identical.

The adjacent `.zip.sha256` and `.manifest.json` record the exact archive/binary SHA256, build identity, binary source commit, documentation commit and toolchain. `SHA256SUMS` hashes every payload except itself. Generated outputs remain local. Optional symbols are not emitted by this tool; test targets, runtime/parser/NVIDIA DLLs, dumps and game symbols never enter the player archive.

Windows workflows configure compilation, native lifecycle/unit/collector checks, documentation/configuration checks, checksum-verified redacted Gitleaks 8.24.3 scanning and candidate artifacts. They use reviewed full Action commit pins and `windows-2025`. Its current official inventory includes VS2022 x64, CMake and Python; the actual runner agent/toolchain versions must be recorded in executed job logs. Check [GitHub Actions](https://github.com/JrSneed28/WitcherFG/actions) for hosted run results. PR jobs receive read-only permissions and no publication credentials; no workflow activates the installed game or publishes a release.

## Required acceptance before publication

1. Build a clean committed candidate and record its source commit, build-input digest, build ID and toolchain.
2. Run native unit/fixture checks plus actual config/help/version and documentation/schema/link checks.
3. Package once, record the exact ZIP SHA256, and validate fresh extraction, upgrade/config preservation, game routing/rendering/FG or MFG, a guard refusal and complete uninstall/ordinary Xbox restoration against those exact bytes.
4. Record measured gameplay and player confirmation in public validation records. Keep diagnostic-probe, synthetic native fixture and native release scopes separate in [compatibility](compatibility.md).
5. After publication authorization, upload identical accepted bytes to the configured channels and recheck hashes/build IDs. Partial publication remains pending synchronization. Public URLs become links only after their locations exist and are verified.

Keep `release/validation.json` outside the installer. Schema 1 records `version`, `artifact_sha256`, `binary_sha256`, `source_commit`, `input_digest`, `build_id` (the input digest), actual `tag` or an empty string, `publication_ready`, `environment` (windows/gpu/driver/game_package/method), `known_limits`, and five `cases`: fresh_install, upgrade_config, gameplay_fg, guard_refusal and uninstall_restore. Each case needs status Passed and an evidence reference before a record establishes acceptance. An absent record verifies only candidate integrity. A supplied incomplete or different-artifact record refuses acceptance. Optional `github_sha256`/`nexus_sha256` must equal the tested archive hash.

The tested binary's code/build-input digest must remain unchanged if documentation/acceptance commits follow it. Verification does not regenerate documents or require repackaging the tested ZIP solely because an external acceptance document records its checksum. Do not rebuild to embed the archive's own hash. Stable packaging is unavailable until an actual live release decision; fixture records cannot authorize stable claims.

[Release summary](release-summary.md) is the shared player-facing source for channel descriptions; [Nexus BBCode](NEXUS_DESCRIPTION.bbcode) is an unpublished generated draft. Rendering supports headings, paragraphs, flat lists, links, inline code/bold, fenced code (including the canonical indented list example) and simple tables. HTML, images, nested lists, blockquotes and unsupported inline formatting refuse; literal BBCode markup is escaped. [Troubleshooting](troubleshooting.md) owns the symptom/code/cause/action table. Generated player text belongs in package staging, not committed binaries.

External hosting, uploads, publisher messages and private-reporting setup are separate actions. No publication follows merely from successful compilation or a green fixture suite.
