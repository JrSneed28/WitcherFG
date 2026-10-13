# Troubleshoot WitcherFG

Use the final status, code, stage and operation together. The table below is the canonical symptom/code/cause/action source for offline instructions and release summaries. A guard refusal is a restriction, not proof of a gameplay failure. Do not repeat a launch against an existing game session.

| Symptom | Code | Cause or evidence limit | Action |
| --- | --- | --- | --- |
| No visible effect after `Applied` | No dedicated code | Routing does not establish actual frame generation | Check the game mode, visible world and player-confirmed motion; retain the summary. Do not infer generated frames from FPS ratios. |
| FG option greyed out | No dedicated code | The game controls hardware/SDK eligibility; exact cause is not established by the launcher | Record GPU, driver, selected-adapter availability, configured versus active HAGS, and FG/MFG mode. Do not force driver/profile or menu settings through configuration. |
| Invalid options or configuration | `WFG-E-CONFIG_INVALID` | Syntax/schema/type/range/size or guarded config storage refused | Correct/remove the optional config. Use an ordinary fixed local utility folder; retain the failed operation. |
| Unsupported game or imports | `WFG-E-BUILD_UNSUPPORTED` | Compiled identity/fingerprint/import guards did not validate | Keep the game unmodified. Record exact package/build; a new compatibility record requires independent validation. |
| Local DLL conflict | `WFG-E-DLL_CONFLICT` | Application-local dependency/mod routing fails the guard | Preserve existing DLLs and backups. Follow the conflicting mod's own removal/restoration procedure; coexistence remains Untested. |
| Interposer cannot be trusted | `WFG-E-INTERPOSER_UNTRUSTED` | Signed/versioned/hash/backing-image evidence did not validate | Restore the installation through its supported repair path. Do not download a replacement DLL from an arbitrary source. |
| Debugger access denied | `WFG-E-ATTACH_DENIED` | Windows refused required debugger access | Close the game and retry from the current user's session. Retain the native error; automatic elevation is not provided. |
| Graphics startup already happened | `WFG-E-STARTUP_TOO_LATE` | Non-null graphics factory or unsafe layout prevented mutation | Close the game and start WitcherFG before a fresh session. The guard cannot be bypassed. |
| Registered executable cannot be located | `WFG-E-LAUNCH_FAILED` | An installation hint may resolve a pathname limitation only when the registered documents and executable are confirmed present and valid; missing mandatory assets refuse without a prompt | In a real console, enter the supported installation path when prompted. Otherwise retry `WitcherFG.exe --game-path "D:\XboxGames\The Witcher 3\Content"`; add `--diagnose` for read-only diagnosis. Cancelled, unrelated or unverifiable selections refuse before activation. |
| Launch/discovery/collector failure | `WFG-E-LAUNCH_FAILED` | See the recorded stage and operation; collector failures also use this general stable code | Check registered Xbox installation, current-user session, existing game and fixed local storage. Preserve the operation/native code and console result. |
| Import write failed | `WFG-E-IAT_WRITE` | A stopped-process pointer write failed | Follow the reported rollback/abort result; do not retry while the game remains open. |
| Import verification failed | `WFG-E-IAT_VERIFY` | Read-back did not confirm intended pointers | Follow the verified restoration/abort result and retain diagnostics. |
| Page protection restore failed | `WFG-E-PROTECTION_RESTORE` | Original page protection could not be restored as expected | Check the final restoration/abort outcome; close the game before retrying. |
| Rollback could not be verified | `WFG-E-ROLLBACK_ABORT` | Unsafe restoration requires containment of only the fresh owned startup | Independently check the game process. Helper death alone is not target-exit proof. Retain any console/summary result. |
| Log/summary cannot be written | `WFG-E-DIAGNOSTIC_IO` | Local storage access, ownership or bounded write failed | Retain the console summary. Close other utility/collector instances; check fixed local application-data storage. A committed patch is not retried for logging. |
| Runtime observation unavailable | `WFG-W-OBSERVATION_UNAVAILABLE` | Current production SDK catalog is empty, or process/layout access is unavailable | Keep the field Unknown. Inventory/import facts may still exist; enabling Debug does not supply SDK offsets. |
| Activity absent in a bounded observation | `WFG-W-FG_ACTIVITY_NOT_OBSERVED` | Requires valid activity fields; currently exercised by fixtures, not the empty production catalog | Record scope and observation window. Do not promote this warning into proof of general FG failure. |
| Decoded SDK rejection | `WFG-E-FG_REJECTED` | Requires independently validated matching vendor status/code/reason; production layouts currently unavailable | Preserve the actual decoded evidence when supported; never guess a rejection from an unknown field. |
| Black world after temporary routing | No dedicated code | Historical diagnostic probes saw this symptom; the user now reports it resolved | Retain the historical recovery notes below. This report does not verify native candidate gameplay or FG. |

## Manual black-world recovery

The user reported the black screen resolved on 2026-10-12. The resolution is a user report, separately labelled from native artifact gameplay acceptance; its cause and native FG remain unverified. Historical evidence is retained below.

The earlier local Xbox diagnostic probe required a ray tracing reset before normal world rendering and smoother motion were confirmed. In that tested session:

1. Open the game's graphics settings and turn ray tracing off.
2. Return to gameplay and allow the world to render.
3. Turn ray tracing back on and check the world again.

This is a conditional manual workaround, not an automatic launcher feature or a guarantee for another build. If it fails, close the game and check normal Xbox startup. Do not overwrite game DLLs as a recovery step.

## Reports and collection

See [diagnostics](diagnostics.md) for returned exit 0/2/3/4 versus forced watchdog 1460/fallback 124/immediate unsafe helper 3. None of the forced helper codes proves target exit or detach.

Explicit collection with `WitcherFG.exe --collect-support` writes a local support archive. Review its JSON before attachment. Include launcher build ID, game/package identity, Windows/GPU/driver, renderer, FG/MFG mode, mods/versions, error code/stage/operation and exact reproduction steps. Public support hosting is unpublished; private security reporting is unavailable. See [SECURITY.md](../SECURITY.md).
