# Release status and acceptance

`release/project.toml` is the current canonical version/channel/schema metadata. The version is `0.1.0-preview.1`, channel `preview`, diagnostic schema 1. Publication URLs are empty: **unpublished**. No stable release or verified private contact is available.

## Source ZIP and player ZIP

A source ZIP is developer input. Build it using [building](building.md); it is not a portable player package. Native development binaries and synthetic fixture results are also not a player release.

Player ZIP creation, text/BBCode rendering and CI packaging workflows are pending implementation. There is no working release-script command to give yet. The planned player package includes the executable, configuration and offline instructions/notices; its final inventory must come from the implemented packager. Do not copy test targets or NVIDIA binaries into a player package.

## Required acceptance before publication

1. Build a clean committed candidate and record its source commit, build-input digest, build ID and toolchain.
2. Run native unit/fixture checks plus actual config/help/version and documentation/schema/link checks.
3. Package once, record the exact ZIP SHA256, and validate fresh extraction, upgrade/config preservation, game routing/rendering/FG or MFG, a guard refusal and complete uninstall/ordinary Xbox restoration against those exact bytes.
4. Record measured gameplay and player confirmation in public validation records. Keep diagnostic-probe, synthetic native fixture and native release scopes separate in [compatibility](compatibility.md).
5. After publication authorization, upload identical accepted bytes to the configured channels and recheck hashes/build IDs. Partial publication remains pending synchronization. Public URLs become links only after their locations exist and are verified.

These are release requirements, not completed claims. [Release summary](release-summary.md) is the shared player-facing source for channel descriptions; [Nexus BBCode source](NEXUS_DESCRIPTION.bbcode) is an unpublished draft. The future renderer must derive channel output from canonical sources and fail unsupported formatting. [Troubleshooting](troubleshooting.md) owns the symptom/code/cause/action table. Generated player text belongs in package staging, not in committed binaries.

External hosting, uploads, publisher messages and private-reporting setup are separate actions. No publication follows merely from successful compilation or a green fixture suite.
