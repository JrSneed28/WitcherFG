# Contribute to WitcherFG

Use Windows x64, MSVC desktop C++ tools, C++20, Windows SDK, CMake ≥3.24 and Python ≥3.11. [Building](docs/building.md) supplies the actual configure/build/CTest and native documentation-check commands. Keep the vendored parser's provenance and complete license notice.

Use a focused `feature/` or `fix/` branch. Keep changes small and preserve existing formatting; new C++ uses four-space indentation, UTF-8, LF and a final newline. Do not mix formatting churn with safety changes. Commands and product documentation must describe implemented behavior.

Run the affected native checks and the applicable full CTest suite in Release and Debug. For docs/config/schema changes, run documentation checks with both native executable environment variables set. Record commands, source/build identity, configuration, failures and limitations. Never equate compilation or synthetic fixture success with gameplay frame generation.

A pull request should explain the concrete behavior change, supporting evidence, tests and remaining risks. Include relevant screenshots for visual gameplay claims. Safety changes need transaction/rollback/owned-process containment regression coverage; privacy changes need sanitizer/storage tests.

New compatibility records require exact package/executable identity, measured fingerprints/import RVAs, the measured import selector form for each import (imported by name, or by ordinal with the ordinal value), dependency provenance, readable pre-factory guard evidence and startup/gameplay validation. A native release claim requires the exact tested artifact SHA256 and build ID. Record FG versus MFG, HAGS configured versus active, selected-adapter availability, driver/OS/resolution, named mods/managers and dates. Reported or Untested observations do not become Verified without reproduction.

Respect existing ignore rules. Keep private diagnostics, support archives, internal investigation and credentials local; do not force-add ignored material. Review sanitized bundles before sharing. [Security reporting](SECURITY.md) is separate from ordinary bugs, and hosted contribution/support locations are currently unpublished.
