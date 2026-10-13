# WitcherFG

WitcherFG is a portable Windows x64 launcher that attempts guarded, temporary Streamline import routing for one Xbox PC build of The Witcher 3.

<!-- BEGIN WFG STATUS -->
**Status: unpublished preview, 0.1.0-preview.1.** A candidate can be packaged locally. Exact-candidate gameplay/install/upgrade/uninstall acceptance is Pending. No stable release is available.
Release: unpublished. Support: unpublished.
<!-- END WFG STATUS -->

The earlier local diagnostic probe restored FG/MFG on an RTX 5070 Ti. That observation does not verify this launcher or a release ZIP. `Applied` and `RoutingAlreadyPresent` describe import routing; they do not prove gameplay frame generation.

## Quick start

Developers can [build the executable](docs/building.md) and [package a local candidate](docs/releasing.md). Public downloads and player acceptance remain Pending. For a built copy, close the game and keep the utility in its own folder on a fixed local drive, outside the game directory. Run from PowerShell in that folder:

```powershell
.\WitcherFG.exe --help
.\WitcherFG.exe --version
.\WitcherFG.exe
```

The last command activates the current user's registered Xbox game and attempts session routing. Read its final status and action. Unknown builds, existing sessions, conflicting DLLs and late graphics startup refuse. See [installation and upgrades](docs/installation.md) before using a future player ZIP.

## Compatibility and verification

The compiled record is for Xbox package `CDPROJEKTSA.Witcher3GOTY` version `5.0.4.0`, AMD64. The observed game menu version was 5.01. [Compatibility](docs/compatibility.md) separates diagnostic-probe evidence, native fixtures and release acceptance. Other stores, GPUs, drivers and mod managers remain Untested.

Check rendering and player-confirmed motion separately from routing. FPS ratios, menu toggles and loaded DLLs do not establish generated-frame activity. Production SDK diagnostic layouts are currently unavailable; missing observations remain Unknown. [Diagnostics](docs/diagnostics.md) explains the limits and normal versus forced exit codes.

## Configuration and support

The optional executable-relative `FrameGenFix.toml` uses schema 1. Error logging is the default; automatic runtime observation is Off. Copy the [example configuration](config/FrameGenFix.toml) beside the executable only if needed. See [configuration](docs/configuration.md).

Use [troubleshooting](docs/troubleshooting.md) for stable codes and manual black-world recovery. Explicit local support collection is available:

```powershell
.\WitcherFG.exe --collect-support
```

Review the archive before sharing. Nothing is uploaded automatically. Ordinary reports should include build ID, game/package version, environment, mode, mods, code/stage and reproduction steps; [the bug-form source](.github/ISSUE_TEMPLATE/bug_report.yml) lists the fields. Public issue hosting and private security reporting are unavailable until configured. Read [SECURITY.md](SECURITY.md) before handling a vulnerability.

## Developer and player references

- [Building and tests](docs/building.md), [technical behavior](docs/TECHNICAL.md), [contribution policy](CONTRIBUTING.md).
- [Complete uninstall](docs/uninstall.md), [release process and source versus player ZIP](docs/releasing.md), [changelog](CHANGELOG.md).
- [Windows fixture results](docs/validation/windows-fixture-results.md), [shared release summary](docs/release-summary.md).

Project-owned code uses the [MIT License](LICENSE). Vendored parser licensing and the shipped-component boundaries are in [third-party notices](THIRD_PARTY_NOTICES.md).
