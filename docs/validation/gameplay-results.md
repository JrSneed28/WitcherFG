# Native candidate acceptance — October 12, 2026

This candidate is not ready for publication. Native routing succeeded in one observed launch, but Windows Defender removed the installed launcher and further live acceptance stopped. Gameplay frame generation has not been verified for this artifact.

## Exact artifact

| Property | Value |
| --- | --- |
| Version | 0.1.0-preview.1 |
| Binary source commit | `4dc2688b9de8e6b63f2444973354c7c30ee56b00` |
| Build input digest | `d5dc3ae8f177845c15041ddbb692cb38c23609119cc22a8a7f2dd68beefa825d` |
| Installer SHA256 | `f23bc377c77893635788735048b1f7053833e6de638057866f6f41f873806477` |
| Executable SHA256 | `e4a1558d1a752fe1d94743a47137871a3a604a072680bc1362aa99ba013e1a39` |
| Build | Clean native Windows x64 Release, static C++ runtime |

The installer contains 11 entries, including ten hashed payload files. Independent ZIP CRC and every payload SHA256 check passed. No game or NVIDIA binaries are included.

## Completed checks

Clean committed Release and Debug builds each passed all 19 registered CTest gates and all 22 native documentation checks, with no failures or skips. The final process regression group passed 63 tests. All 26 repository-target compiler command logs used `/WX`; the CMake compiler-identification probe is recorded separately. The final integrated code review and scoped fix review passed specification and quality gates.

Manual game-path handling accepts a registered file/folder hint with Unicode support. Tests reproduce ordinary long-path discovery failure, recover with a handle-associated ordinary folder hint, preserve the independently validated process location through inventory refresh, and refuse unrelated or changed identities. Missing required assets do not trigger an ineffective prompt. These checks do not establish universal long-path module support or successful human console interaction.

The actual ZIP was extracted into a separate utility folder. Installed help, version and default configuration checks passed without activation. A same-candidate new-folder upgrade rehearsal preserved a validated custom configuration and its backup bytes; this does not claim upgrade testing against a previous published release.

## First native launch

The launcher summary recorded `Applied`, no failure, and 44,425 ms elapsed, completing at `2026-10-13T03:47:40Z`. Current-run events included completed build validation, the pre-factory guard and debugger detach.

A previously qualified read-only observer pinned the same fresh process and captured 910 qualified samples. Every import slot had 24 observed native samples and 359 interposer samples with successful debugger queries reporting no debugger. All four native-to-interposer transitions were ordered correctly. This establishes routing and detach observations; it does not establish SDK interpolation, visible gameplay or five-start reliability.

The acceptance wrapper stopped because Windows reported the executable through two path spellings: its registered WindowsApps location and its physical Xbox content location. A separate read-only check confirmed the same executable file identity, fresh creation identity, current user/session and registered package. No input was sent. The owned game was subsequently closed through its normal window close request, and exit was observed without forced termination.

Windows Defender recorded a successful action against the installed `WitcherFG.exe`, classified as `Behavior:Win32/DefenseEvasion.A!ml` (threat ID `2147738096`). The installed executable disappeared after the routing run. The detection is recorded as observed; no claim that it is harmless or a false positive is made. No protection setting, exclusion, restore, re-extraction workaround or repeated live launch was performed after this detection.

## Remaining acceptance

| Case | Result |
| --- | --- |
| Fresh utility install/help/version/default config | Passed for the exact ZIP |
| Config-preserving upgrade | Passed same-candidate rehearsal |
| First routing and detach observation | Observed Applied; independent four-slot transitions |
| Five independent fresh starts | Incomplete; one attempted routing run |
| FG Off/standard/maximum/Dynamic and visible output | Not tested |
| Qualified SDK/progress/interpolation evidence | Unavailable; routing alone is insufficient |
| Renderer/load/window transitions | Not tested |
| Actual unsupported-build/loader refusal | Pending; fixture coverage is separate |
| Complete uninstall and ordinary Xbox restart | Pending |
| Publication readiness | False |

The user's report that the previous black-screen issue is fixed remains separately recorded. It does not replace artifact-bound gameplay evidence. Other hardware, drivers, stores and mod managers remain untested; trust-qualification startup cost, typed-console success, mid-wait cancellation and full long-path module inventory remain explicit limits.

The artifact-bound record is [release/validation.json](../../release/validation.json). Resume requires an explicit decision about the Defender detection, a reviewed correction to the private controller's alias comparison, and the remaining live acceptance rows. Preserve the original detection and stopped-run evidence. Any changed installer bytes require a new SHA256 and repetition of affected artifact checks.
