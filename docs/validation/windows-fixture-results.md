# Windows disposable-fixture validation

For maintainers reviewing launcher lifecycle safety. Results below concern synthetic Windows children and assume familiarity with the transaction design.

Tested on 2026-10-10. Native x64 Release and Debug each passed **18/18 CTest gates**, **214/214 main-runner tests**, and **17/17 lifecycle groups covering 25 physical cases**. The separate transaction runner passed 25/25, native debug boundary 14/14, and support runner 44/44; independent support ZIP checks also passed.

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

Replace `<native-build>` with that configured build directory. The recorded complete Release and Debug runs used `ctest --test-dir <native-build> -C <configuration> --no-tests=error --output-on-failure -V`, followed by the complete `wfg_tests.exe` runner. The gate launches only its own fixture/helper executables.

## What the fixture proves

The controller creates the target and retains its process handle. The target publishes its PID, creation time, synthetic PE layout, and four pointer slots over an inherited anonymous pipe. Each slot occupies a separate page; protections alternate `PAGE_READONLY` (2) and `PAGE_READWRITE` (4). Native and replacement exports return distinct values. After execution resumes, the target reports all four pointer values, `VirtualQuery` protections, and actual function-call results.

The helper uses the production debugger, transaction, watchdog, validation, coordinator, and native memory adapters. Test-only construction supplies an independently checked live child pin, with identity checked before and after rights-restricted duplication. Synthetic trust/compatibility inputs describe fixture pages only. No game or NVIDIA binary is loaded by the fixture.

Mutation-death cases require successful native TRUE policy, a held stop, acknowledged native writes, exact post-write pointer bytes, and independently observed target exit. Missing execution output alone cannot pass. Continuation cases require actual target callback results and restored protections.

An unrelated sentinel runs in every case. Its original pointers, protections, and nondebugged execution are checked. It must remain alive after helper and target cleanup; only then does the controller request the sentinel’s separate cooperative exit. Sentinel handles are never inherited by the helper, and cleanup never terminates the sentinel.

| Case | Required native observation | Release | Debug |
| --- | --- | --- | --- |
| Inspection helper death | Originals execute; helper died under FALSE | Pass | Pass |
| Inspection debugger-thread death | Originals execute while helper remains alive after debugger-thread exit | Pass | Pass |
| Death after write 1 | Exactly 1 acknowledged writes; OS target exit; no mixed execution | Pass | Pass |
| Death after write 2 | Exactly 2 acknowledged writes; OS target exit; no mixed execution | Pass | Pass |
| Death after write 3 | Exactly 3 acknowledged writes; OS target exit; no mixed execution | Pass | Pass |
| Death after write 4 | Exactly 4 acknowledged writes; OS target exit; no mixed execution | Pass | Pass |
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

## Bounds and exit interpretation

Each controller case reserves 8.5 seconds for setup/observation and 1.5 seconds for bounded cleanup. The fixture’s deadline-only watchdog cases use a 200 ms work deadline plus the production two-second cleanup grace. The blocked-observation deadline case selects a one-second observation duration plus that grace. These shorten fixture execution; they do not change the production 30-second discovery or 60-second total-work policy.

External cancellation is timestamped immediately before `request_stop()`. Tests allow a documented 500 ms scheduling margin: helper exit must occur between 1.8 and 2.5 seconds after the request. The independent observation case must exit 2.8–3.5 seconds after blocking. These are measured scheduling checks on this host, not hard-real-time guarantees.

| Blocked case | Release request → exit | Debug request → exit |
| --- | ---: | ---: |
| Blocked observation acquisition, cancellation | 2015 ms | 2031 ms |
| Blocked diagnostic output | 2031 ms | 2015 ms |
| Blocked final stdout | 2015 ms | 2032 ms |
| Read-only cancellation | 2032 ms | 2032 ms |

Native watchdog termination observed exit code `1460`; its fallback `124` is separately accepted by assertions. Immediate unsafe containment uses helper exit `3` without a returned launcher result. Neither code proves target exit or detach. Normal returned status codes remain `0/2/3/4`; rollback’s returned `3` is recorded independently from the disposable helper driver’s exit.

## Tested artifacts

These are development artifacts from source base `902a300a79164c3af9b082ebe08dd65fb0b3c8fe`, with `dirty=true`, `archive=false`. They precede the Task 10 commit and are not release or live-game acceptance artifacts.

| Configuration | Input digest |
| --- | --- |
| Release | `d8012ef0a44b7756da6389c0c9e78da16aa923af49491b6c58fa918df48ff3ee` |
| Debug | `6bb89bbb144910415654a7ba86ed7260b46422c381b102d0ee92305a35d2835c` |

| Configuration | Artifact | Bytes | SHA256 |
| --- | --- | ---: | --- |
| Release | `wfg_lifecycle_helper.exe` | 696832 | `684db83de89f5de3f3ae0a8daab7e37824402b65dec0c2e70278b0a746971870` |
| Release | `wfg_lifecycle_tests.exe` | 326656 | `75e7ae4ccf3ec63964aac87eaf31f6543c5aefac63aed2a58b47ad225bcefcce` |
| Release | `wfg_fixture_target.exe` | 162304 | `50d6f83591bb5f08937889b3b0a95330d36c44c4979e1747b56dc46cbbe2ea5c` |
| Release | `WitcherFG.exe` | 817664 | `716e4648c957907fa1a75be194cc525e4aa2c6fe4063fdea473f1ee78106df9e` |
| Debug | `wfg_lifecycle_helper.exe` | 4302848 | `b3e629a90f0544e4c7648a4e9782a7786c8ad51e0f21e123a2e8bdc4ad759c64` |
| Debug | `wfg_lifecycle_tests.exe` | 2778624 | `25af0745b27d57c3d128eeb30ad15d77233ad85499a716ec35e95ee89c4e3570` |
| Debug | `wfg_fixture_target.exe` | 1301504 | `e9d1a82350df99ed29c6d4ad228e379a8bf62e7ef1c46193f0075ac1bee48f12` |
| Debug | `WitcherFG.exe` | 4225024 | `1d658cf1777c715dadfe2b6847b50929de4264ebb74772ce2a3e869fea0ddbb8` |

Production API probes compiled with `/WX`. Both production executables passed ASCII/UTF-16 fixture-string scans; `wfg_core`, `wfg_launch`, and `wfg_support` passed symbol scans. Positive controls found both native adapter factories in the test core. Lifecycle factories and fixture hooks exist only in the separately compiled test core/helper.

## Failed attempts and limits

- Initial compiled RED: 0/15 groups; missing readiness could not pass.
- First native implementation: 14/15. A reduced-handle write to a read-only page returned Windows 998 before access-mask validation. The corrected test independently verifies a writable page and a successful same-value owned-handle write, then requires reduced-handle errors 5 for write and protection changes.
- Strengthened clean RED: 11/17; six missing positive observations were added before implementation. An earlier incremental run used a stale harness object after an observation-structure change; its 7/17 result is excluded. Clean rebuilds supplied subsequent evidence.
- Default configuration/logger provenance remains covered by the complete native suite. The lifecycle locality case exercises the collector’s classifier/root-open seam without creating a network mount, mapped drive, redirected profile, or registry change.
- Blocked-I/O faults use actual anonymous-pipe reads/writes at fixture environment/sink boundaries. They prove the production watchdog and completion lifetime under real blocked OS I/O; they do not reproduce every possible filesystem, driver, or process-acquisition hang.
- Synthetic module trust facts do not test real Authenticode/package eligibility. No registered game activation, gameplay, GPU/driver compatibility, generated-frame count, NVIDIA binary, settings/profile change, upload, or publication was part of this gate.
- These results cover this Windows host and these bytes. Re-run after lifecycle, rights, transaction, watchdog, output-lifetime, or fixture changes. Actual game compatibility and frame-generation acceptance remain separate gates.
