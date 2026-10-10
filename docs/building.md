# Build and test WitcherFG

These commands build developer binaries. They do not create an accepted player ZIP or establish gameplay frame generation.

## Prerequisites

Use Windows x64, MSVC with desktop C++ tools and Windows SDK, CMake 3.24 or newer on PATH, and Python 3.11 or newer on PATH. CMake requires MSVC x64 and C++20 and selects the static C++ runtime. The TOML parser is vendored; configuring does not download it.

Native fixture validation used Visual Studio 18 2026/MSVC 19.51.36260.0, Windows SDK 10.0.28000.0 and Python 3.12 on Windows build 26200. The tracked `windows-x64` preset targets Visual Studio 17 2022; that preset is preserved, but it was not the installed toolchain used for those results.

Obtain the repository source or a source ZIP and extract/open the repository root in a Windows shell. A source ZIP contains source, tests and documentation, not a prebuilt player installation.

## Actual native build commands

For the installed VS18 toolchain, from the repository root:

```powershell
cmake -S . -B build/native -G "Visual Studio 18 2026" -A x64
cmake --build build/native --config Release --parallel 2
ctest --test-dir build/native -C Release --no-tests=error --output-on-failure
.\build\native\Release\WitcherFG.exe --help
.\build\native\Release\WitcherFG.exe --version
```

Configuration requires a Python interpreter; if discovery chooses the wrong installation, pass CMake's `Python3_EXECUTABLE` cache setting with the full path of your installed interpreter. Use the same build directory/configuration consistently. Help shows the supported modes; version includes source commit, build-input digest, channel, dirty/archive booleans and diagnostic schema. CTest must report no failures. Dirty/source-archive identity is development provenance; candidate packaging refuses it.

The VS17 preset is available for developers with that generator installed:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --test-dir build/windows-x64 -C Release --no-tests=error --output-on-failure
```

Its configure/build/test walk is Pending on a VS17 machine. For Debug with the VS18 build directory, repeat build/CTest using `--config Debug` and `-C Debug`. Do not describe a compilation pass as a gameplay test.

## Documentation and native parser consistency

After a Release build, from the repository root:

```powershell
$env:WFG_TEST_RUNNER = (Resolve-Path .\build\native\Release\wfg_tests.exe).Path
$env:WFG_LAUNCHER = (Resolve-Path .\build\native\Release\WitcherFG.exe).Path
python -m unittest discover -s tests -p documentation_tests.py -v
```

This runs public-link, evidence-scope, code-coverage, license/form and actual native config/help/version checks. The test runner's configuration snapshot uses a temporary fixed-local directory and never activates the game. Without those environment variables the three native integration cases explicitly skip; such a run does not close the native documentation gate.

See [Windows fixture results](validation/windows-fixture-results.md) for measured synthetic lifecycle scope. The test fixture target is disposable and distinct from the game. [Releasing](releasing.md) documents rendering, packaging and verification commands. Configured hosted workflows do not establish an executed CI result.
