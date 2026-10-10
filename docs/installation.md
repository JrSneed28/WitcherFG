# Install or upgrade WitcherFG

These player procedures are **Pending acceptance on the exact packaged candidate**. Packaging and public downloads are not available yet. Existing native fixture results do not establish reliable game startup, mod coexistence or gameplay.

## Prerequisites

- Windows x64 and the current user's registered Xbox PC installation of The Witcher 3.
- The exact compiled Xbox package/build record described in [compatibility](compatibility.md). The game still controls hardware eligibility and FG/MFG support.
- A separate utility folder on a fixed local physical volume. Network drives, removable drives, SUBST mappings and reparse-point paths can be refused. The Windows local application-data location must also meet these storage restrictions.
- A future tested player ZIP with recorded SHA256 and build ID. A source ZIP contains developer files and is not a player installation.

The game executable `bin\Gaming.Desktop.x64\witcher3.exe` and its registered Xbox installation path are inspection targets. Extract the utility elsewhere. Keep existing `dxgi.dll`, `d3d12.dll`, interposer and mod-loader files intact. The launcher does not replace game files or provide chainloading.

## Fresh extraction

1. Obtain the player ZIP only when a real release location and its checksum are published. Compare its SHA256 with the published record using PowerShell `Get-FileHash -Algorithm SHA256` on the downloaded file. No published checksum exists yet.
2. Close The Witcher 3 and other WitcherFG instances.
3. Extract the ZIP into a new, separate versioned utility folder on a fixed local drive. Retain all packaged notices and instructions.
4. If needed, copy the supplied example `FrameGenFix.toml` beside `WitcherFG.exe`. Keep its safe defaults for the first attempt.
5. Open PowerShell in the utility folder and run:

   ```powershell
   .\WitcherFG.exe --version
   .\WitcherFG.exe
   ```

6. Read the console result. `Applied` or `RoutingAlreadyPresent` confirms only this session's routing. Check a visible world, requested FG/MFG mode and motion separately. See [diagnostic limits](diagnostics.md) and [troubleshooting](troubleshooting.md).

The launcher uses registered activation for the current user. Starting the game first can produce `AlreadyRunning`; a factory created before inspection produces `StartupTooLate`. Neither is a reason to change game permissions or run automatic elevation.

## Upgrade while preserving configuration

1. Close the game and every utility instance.
2. Keep the previous utility folder and make your own copy of its `FrameGenFix.toml` before changing it.
3. Extract the new player ZIP into a new folder. Review [changelog](../CHANGELOG.md) and [configuration schema](configuration.md) before copying your configuration beside the new executable.
4. Check version, configuration and the launch result. The launcher neither migrates nor rewrites configuration automatically.
5. Remove the old utility folder only after the new version passes your checks. A backup is a copy you created, not a game-file backup made by WitcherFG.

## Other mods and managers

No named mod coexistence or manager version is verified yet. Vortex and other managers are Untested; use manual extraction. Follow each mod's own backup/removal instructions when a conflict occurs. Preserve its files and backups. Restoring a mod's files is the user's separate action; WitcherFG cannot chainload around a conflict.

If the world is black, use the manual recovery procedure in [troubleshooting](troubleshooting.md). To remove the utility completely, follow [uninstall](uninstall.md).
