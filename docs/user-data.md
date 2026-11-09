# Saves and settings: the portable `user_data` folder

Everything the port writes for the player lives in one folder, `user_data`, next to the program.

```
TurtlesInTimeReshelled\
    tmnt_turtles_in_time_reshelled.exe
    tmnt_turtles_in_time_reshelled.toml
    game\
    logs\
    user_data\
        settings.toml           launcher choices
        online_identity.ini     this installation's online identity (private)
        bug_reports.json        reports sent from this PC, with their receipts
        0000000000000000\
            584109D2\           installed DLC
        achievements\           unlocked achievements (584109D2.toml)
        cache\                  ReXGlue's shader cache
```

`bug_reports.json` holds the ticket, receipt, description, log name, game facts, status and developer message of the
reports sent from this PC; see [bug-reports.md](bug-reports.md). Keep it private: the receipts prove ownership of those
reports. An interrupted save leaves the previous file; a damaged one is renamed `bug_reports.json.damaged`.

Leaderboard scores are kept at `584109D2\live\stats.toml` under `user_data`; only the local player's submitted best score is stored.

## Offline Xbox Live data

This port has no Xbox Live service or Microsoft account, and no remote leaderboard. Online play with friends goes through the
project's own service ([online.md](online.md)), not Xbox Live. Leaderboard requests are answered from the local stats file, and
player display names come from the launcher settings without changing the profile, XUID or save identity. The game's original
warning about scores not being updated to Xbox Live leaderboards describes a service this port does not provide.

The layout inside `user_data` is ReXGlue's own, unchanged: only its root moved. Copying the whole program folder to
another PC carries saves, achievements, DLC and settings with it. `user_data` is created the first time it is
needed and is never part of a release download. `game` stays separate and is not player data. Logs stay in `logs` next to
the program (in `user_data\logs` only when an AppImage's folder is not writable).

## Where the folder is

| System | `user_data` is in |
| --- | --- |
| Windows | the folder that holds `tmnt_turtles_in_time_reshelled.exe` |
| Linux AppImage | the folder that holds the AppImage file (never inside its read-only mount) |
| Linux, outside an AppImage | the folder that holds the program |

This is the same folder as `game` and `logs` (`PortFolder()` in `src/platform.cpp`). `--user_data_root=<folder>` wins
over everything: that folder is used as it is, nothing is migrated and `user_data` is not created.
`tools\build.ps1 -Run` passes it to keep a development profile in `build\user_data`. `--cache_root` likewise keeps the
cache where it names; without it the cache follows the user data root (`<user data>\cache`).

The About page's **Saves and settings** row opens the folder in use and says which one it is.

## Migration from ReXGlue's default folder

Without this port's portable folder, ReXGlue keeps this data in the system's user folder (its default user data root):

| System | Previous folder |
| --- | --- |
| Windows | `Documents\tmnt_turtles_in_time_reshelled` |
| Linux | `$XDG_DATA_HOME/tmnt_turtles_in_time_reshelled`, normally `~/.local/share/tmnt_turtles_in_time_reshelled` |

If that folder holds data, the next start **moves** it into `user_data` before anything reads or writes it, in this
order (`SelectUserData` in `src/user_data.cpp`):

1. If `user_data` does not exist (or holds no files, only empty folders) and the old folder holds files, rename the old
   folder to `user_data`.
2. If the rename is not possible, for example because Documents and the game are on different drives, copy the old
   folder into `user_data.migrating` next to `user_data`, check that the copy has the same folders and files with the
   same sizes, then rename it to `user_data`.
3. Only then remove the old folder.

If any step fails, the old folder is left as it was, no partial `user_data` is ever active, `user_data.migrating` is
removed, the old folder is used for that run, one warning is logged, and the move is tried again at the next start. If
only removing the old folder fails, `user_data` is used and the log says the old folder remains.

| Situation | Result |
| --- | --- |
| No old data, no `user_data` | `user_data` is created and used |
| Old data, no `user_data` (or an empty one) | old data moves in, as above |
| `user_data` and the old folder both hold data | `user_data` is used; the old folder is neither merged nor deleted; one warning |
| `user_data` cannot be created or written | the system folder is used for that run; one warning |
| `--user_data_root` given | that folder is used, nothing moves |

The log carries one `User data:` line for each start.

## Updating

The launcher updates itself and keeps `game`, `user_data`, `logs` and caches. By hand, extract a newer ZIP over the old folder,
or move `user_data` (and `game`) into the new folder. Deleting the old program folder deletes `user_data` with it.

## Packaging

`tools\package-release.ps1` and `tools\package-linux.sh` reject `user_data` and `user_data.migrating`, along with
`cache`, `logs`, `584109D2`, `settings.toml`, game files and save or log file types.

## Tests

`tests\user_data_tests.cpp` (target `tmnt_user_data_tests`) covers choosing the root, the move by rename and by copy, the
collision and empty-folder rules, failures at each step, the fallback and the `--user_data_root` precedence, using
temporary folders only. `tools\test-release-check.ps1` checks on scratch folders that the release check accepts a fresh
extraction and refuses `user_data`, its contents, `user_data.migrating`, `cache`, logs and game files.
