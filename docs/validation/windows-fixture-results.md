# Windows disposable-fixture validation

For maintainers reviewing launcher lifecycle safety. Results below concern synthetic Windows children and assume familiarity with the transaction design.

Tested on 2026-10-10. After the independent-read and cleanup review fixes, native x64 Release and Debug each passed **18/18 lifecycle groups**: the 25 existing physical cases plus two new cleanup failure scenarios, in **1/1 selected lifecycle CTest gate**. Each final run recorded all ten independent controller read rounds across the four mutation-death cases.

The earlier full-suite baseline passed 18/18 CTest gates, 214/214 main-runner tests, 25/25 transaction tests, 14/14 native debug-boundary tests, and 44/44 support tests plus independent ZIP checks. Those are retained baseline results; this fixture-only correction reran the changed physical gate rather than the unchanged full suite.

## Contents

- [Environment and reproduction](#environment-and-reproduction)
- [What the fixture proves](#what-the-fixture-proves)
- [Bounds and exit interpretation](#bounds-and-exit-interpretation)
- [Tested artifacts](#tested-artifacts)
- [Failed attempts and limits](#failed-attempts-and-limits)

## Environment and reproduction

- Windows version: `10.0.26200.0`; Windows SDK `10.0.28000.0`.
- Visual Studio 18 2026 / MSVC `19.51.36260.0`, x64, C++20, static CRT (`/MT`, `/MTd`).
- Native builds used a Windows-local scratch checkout with SHA256 equality for 68 source/input files before and after each run. The repository’s VS17 preset was preserved; the local harness explicitly selected VS18.

From a configured native build directory, the physical gate is:

```powershell
ctest --test-dir <native-build> -C Release -L lifecycle --no-tests=error --output-on-failure -V
```

Replace `<native-build>` with that configured build directory. The retained earlier complete Release and Debug runs used `ctest --test-dir <native-build> -C <configuration> --no-tests=error --output-on-failure -V`, followed by the complete `wfg_tests.exe` runner. The gate launches only its own fixture/helper executables.

## What the fixture proves

The controller creates the target and retains its process handle. The target publishes its PID, creation time, synthetic PE layout, and four pointer slots over an inherited anonymous pipe. Each slot occupies a separate page; protections alternate `PAGE_READONLY` (2) and `PAGE_READWRITE` (4). Native and replacement exports return distinct values. After execution resumes, the target reports all four pointer values, `VirtualQuery` protections, and actual function-call results.

The helper uses the production debugger, transaction, watchdog, validation, coordinator, and native memory adapters. Test-only construction supplies an independently checked live child pin, with identity checked before and after rights-restricted duplication. Synthetic trust/compatibility inputs describe fixture pages only. No game or NVIDIA binary is loaded by the fixture.

Mutation-death cases require successful native TRUE policy, a held stop, and helper-native readback. After every write 1 through N, the helper waits while the controller independently reads all four actual slots through its retained target handle, checks their expected values and live held state, then acknowledges. A 500 ms missing-ack timeout fails the case; the next write or selected helper death cannot precede acknowledgement. Target exit is independently observed. Missing execution output alone cannot pass. Continuation cases require actual target callback results and restored protections.

An unrelated sentinel runs throughout the fixture. In the 25 lifecycle cases, its original pointers, protections, and nondebugged execution are checked. It must remain alive after helper and target cleanup; only then does the controller request the sentinel’s separate cooperative exit. Sentinel handles are never inherited by the helper, and cleanup never terminates the sentinel. Construction cleanup ownership is installed immediately after spawn, before layout validation can throw. Explicit helper/target cleanup checks termination results and observes exit before marking a child stopped or accepting the sentinel proof.

| Case | Required native observation | Release | Debug |
| --- | --- | --- | --- |
| Inspection helper death | Originals execute; helper died under FALSE | Pass | Pass |
| Inspection debugger-thread death | Originals execute while helper remains alive after debugger-thread exit | Pass | Pass |
| Death after write 1 | All four slots independently read after every write through 1; OS target exit | Pass | Pass |
| Death after write 2 | All four slots independently read after every write through 2; OS target exit | Pass | Pass |
| Death after write 3 | All four slots independently read after every write through 3; OS target exit | Pass | Pass |
| Death after write 4 | All four slots independently read after every write through 4; OS target exit | Pass | Pass |
| Verified commit | Four replacements execute; original protections; detached target survives | Pass | Pass |
| Verified rollback | Four originals execute; original protections; six native writes including recovery | Pass | Pass |
| Unverifiable rollback | Held unsafe target exits; no continuation or unsafe cleanup | Pass | Pass |
| Native termination denied | Real native error 5; parent sees target alive before helper death, then exit | Pass | Pass |
| Initial FALSE policy failure | Real attach with default TRUE; failed FALSE/termination; quiet immediate containment | Pass | Pass |
| Detach retry | Actual unrelated exception serviced before successful retry | Pass | Pass |
| Detach deadline | Actual unrelated exception serviced; committed target survives helper exit | Pass | Pass |
| Unrelated exception | Target exception handler executes via NOT_HANDLED | Pass | Pass |
| Inspection watchdog | Helper exit 1460; original target resumes | Pass | Pass |
| Mutation watchdog | Helper exit 1460; armed target exits | Pass | Pass |
| Blocked observation acquisition, cancellation | Reduced native acquisition, then real blocked pipe; target survives | Pass | Pass |
| Blocked diagnostic output | Real blocked diagnostic pipe; detached target survives | Pass | Pass |
| Blocked final stdout | Production-shared RunLauncherToConsole final output blocks; target survives | Pass | Pass |
| Read-only cancellation | Real reduced adapter/read; zero attach, transaction write, abort calls | Pass | Pass |
| Support collection | Actual local collector; zero attach, transaction write, abort calls | Pass | Pass |
| Storage locality refusal | Injected nonlocal classification rejected before native root open | Pass | Pass |
| Observed handle identity/rights | Writable positive control succeeds; reduced write/protect fail with error 5 | Pass | Pass |
| Blocked observation, independent deadline | One-second observation guard plus two-second grace; no external cancellation | Pass | Pass |
| Exited observed target | Pinned target exits cooperatively; duplicate/open reject exited identity | Pass | Pass |
| Malformed-layout construction failure | Retained independent pin observes exit immediately after constructor unwinding | Pass | Pass |
| Denied cleanup and recovery | Native error 5 surfaces while target remains alive; original full-right pin then observes exit | Pass | Pass |

## Bounds and exit interpretation

Each controller case reserves 8.5 seconds for setup/observation and 1.5 seconds for bounded cleanup. Total time is measured through verified helper/target exit and separate cooperative sentinel teardown, and must not exceed 10 seconds. The combined cleanup regression has the same measured total limit. The fixture’s deadline-only watchdog cases use a 200 ms work deadline plus the production two-second cleanup grace. The blocked-observation deadline case selects a one-second observation duration plus that grace. These shorten fixture execution; they do not change the production 30-second discovery or 60-second total-work policy.

External cancellation is timestamped immediately before `request_stop()`. Tests allow a documented 500 ms scheduling margin: helper exit must occur between 1.8 and 2.5 seconds after the request. The independent observation case must exit 2.8–3.5 seconds after blocking. These are measured scheduling checks on this host, not hard-real-time guarantees. Longest final case totals were 3094 ms (Release) and 4515 ms (Debug); the combined cleanup regression took 984 ms and 969 ms respectively.

| Blocked case | Release request → exit | Debug request → exit |
| --- | ---: | ---: |
| Blocked observation acquisition, cancellation | 2032 ms | 2016 ms |
| Blocked diagnostic output | 2015 ms | 2016 ms |
| Blocked final stdout | 2016 ms | 2031 ms |
| Read-only cancellation | 2031 ms | 2031 ms |

Native watchdog termination observed exit code `1460`; its fallback `124` is separately accepted by assertions. Immediate unsafe containment uses helper exit `3` without a returned launcher result. Neither code proves target exit or detach. Normal returned status codes remain `0/2/3/4`; rollback’s returned `3` is recorded independently from the disposable helper driver’s exit.

## Tested artifacts

These are development artifacts from source base `e54eeca7ba0626222ae1c5d1be29e5baed00b7bc`, with `dirty=true`, `archive=false`. They precede the Task 10 FIX1 commit and are not release or live-game acceptance artifacts.

| Configuration | Input digest |
| --- | --- |
| Release | `6e10d608f9ab98db7523e2715dd17aaf31facef57c9aa675b97bc00788d4d575` |
| Debug | `40c4ffeeaa78e2fc8e3e6e12e38cc64943903f55ac1f0c3da01ba83f26b76772` |

| Configuration | Artifact | Bytes | SHA256 |
| --- | --- | ---: | --- |
| Release | `wfg_lifecycle_helper.exe` | 696832 | `abb1f7b7f072f245d56583f37739e951089af67f74c3d747126c300a2203b8c8` |
| Release | `wfg_lifecycle_tests.exe` | 335360 | `7c55e620ee6fe53aee072299f812e631aecf806a6619b8642ca0f47cee5a8efc` |
| Release | `wfg_fixture_target.exe` | 161792 | `6998ca4533ef86393434845862cb899ca0502a641071fb69f4c28cb1383f3edd` |
| Release | `WitcherFG.exe` | 817664 | `9444932e79c7d3ae24ed373b327491c705d8c7f1d070b98ecc847407fa7e4e63` |
| Debug | `wfg_lifecycle_helper.exe` | 4302848 | `3bb8d93f3e38d69dda79a3c0dea372089a14de2ecc77f9788fe8b40fa4362337` |
| Debug | `wfg_lifecycle_tests.exe` | 2791936 | `206784281c35a990749b19daf7a40a0dd1ebc53d4a47d96d567907ff56aad6b1` |
| Debug | `wfg_fixture_target.exe` | 1302016 | `195b87d08f80554d4ed1cd4d518697c51e59c1ebeeebb9e77f1ab933cda0d642` |
| Debug | `WitcherFG.exe` | 4225024 | `a990ebcf0af519b35ea865e3e2cc6e6ef836e2b847cadce330b87216c70c0759` |

Production API probes compiled with `/WX`. Both production executables passed ASCII/UTF-16 fixture-string scans; `wfg_core`, `wfg_launch`, and `wfg_support` passed symbol scans. Positive controls found both native adapter factories in the test core. Lifecycle factories and fixture hooks exist only in the separately compiled test core/helper.

## Failed attempts and limits

- FIX1 compiled RED passed 16/18 groups. It exposed absent controller-native reads and constructor/denied-cleanup exit proof. The malformed-layout child remained alive after constructor failure; denied cleanup was neither surfaced nor retried. Independent full-right fallback pins contained both RED children. Clean GREEN and final Release/Debug passed 18/18 after the scoped fixes. Earlier passing runs remain evidence for their original assertions, not for these previously missing proofs.

- Initial compiled RED: 0/15 groups; missing readiness could not pass.
- First native implementation: 14/15. A reduced-handle write to a read-only page returned Windows 998 before access-mask validation. The corrected test independently verifies a writable page and a successful same-value owned-handle write, then requires reduced-handle errors 5 for write and protection changes.
- Strengthened clean RED: 11/17; six missing positive observations were added before implementation. An earlier incremental run used a stale harness object after an observation-structure change; its 7/17 result is excluded. Clean rebuilds supplied subsequent evidence.
- Default configuration/logger provenance remains covered by the complete native suite. The lifecycle locality case exercises the collector’s classifier/root-open seam without creating a network mount, mapped drive, redirected profile, or registry change.
- Blocked-I/O faults use actual anonymous-pipe reads/writes at fixture environment/sink boundaries. They prove the production watchdog and completion lifetime under real blocked OS I/O; they do not reproduce every possible filesystem, driver, or process-acquisition hang.
- Synthetic module trust facts do not test real Authenticode/package eligibility. No registered game activation, gameplay, GPU/driver compatibility, generated-frame count, NVIDIA binary, settings/profile change, upload, or publication was part of this gate.
- These results cover this Windows host and these bytes. Re-run after lifecycle, rights, transaction, watchdog, output-lifetime, or fixture changes. Actual game compatibility and frame-generation acceptance remain separate gates.
