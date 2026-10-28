# Turtles in Time Re-Shelled XBLA Package Analysis

Technical metadata only, read from the supplied package. No game data is stored in this repository; the package and the
extracted files live under the Git-ignored `game/` directory.

## Original package

| Field | Value |
|---|---|
| Location | `game/<dump folder>/584109D2/000D0000/200337093BB62547E78CBEFBDC92CB761A0C7D9E58` |
| Size | 278,016,000 bytes |
| SHA-256 | `4DA2FA8EBF870710A06EFD5C22CBE39584CA63D7F4FB741702038BBB916724F9` |
| SHA-1 | `2B641C2B5D7B74B0517BDB6F010F73C5DBB03E5F` |
| Format | Xbox 360 STFS, magic `LIVE` |
| Metadata version | 2 (header size `0xAD0E`) |
| Content type | `0x000D0000` (Arcade Title) |
| Content ID | `200337093BB62547E78CBEFBDC92CB761A0C7D9E` |
| Title ID | `0x584109D2` |
| Media ID | `0x23B57DFA` |
| Version / base version | `0x00000103` / `0x00000103` (0.0.1.3) |
| Display name (EN) | `Turtles In Time RS` |
| Description (EN) | `TMNT: Turtles In Time Reshelled` |
| Publisher and title name fields | empty |
| Platform / executable type / disc | 0 / 0 / 0 of 0 |
| License entries | 1 entry: unrestricted ID (all `0xFF`), license bits `0x00000001` |
| Console / profile / device ID | all zero |
| Transfer flags | `0xC0` |
| Thumbnails | 8,084-byte package and title thumbnails (not extracted) |

STFS volume: read-only layout (block separation 1), 67,463 allocated blocks, file table of 2 blocks starting at block 0.
Every file-table entry is stored in consecutive blocks, so the launcher's installer (`src/stfs.cpp`) reads it without
changes. Block hashes and the Microsoft signature were not verified.

Title ID `0x584109D2` is confirmed by the STFS header, the execution info of both executables and `ArcadeInfo.xml`
(`TitleInfo ID="1480657362"`).

## Extracted layout

102 files in 12 directories, 276,096,929 bytes. Extracted with a Python port of `src/stfs.cpp` (scratch tool, not part of
the repository) into `game/extracted/`.

| Path | Size | Notes |
|---|---:|---|
| `default.xex` | 4,808,704 | Title executable |
| `TMNT_r.xex` | 2,002,944 | DLL module (`TMNT_Final_r.dll`), loaded by `default.xex` |
| `Data/TMNT.bf` | 267,987,050 | Game data archive, magic `ABE\0` |
| `Data/Cache/` | 3 files, 288,745 | `FxDescription_360.fxd`, `fxlist_360.fxl`, `keyList_360.kl` |
| `Data/shaders/` | 81 files, 802,757 | HLSL effect sources (`.fx`, `.fxh`, `.fxm`), including `core/Bink.fx` |
| `ArcadeInfo.xml` | 4,768 | UTF-16 XBLA metadata: 5 locales, 12 achievements (IDs 1 to 12), 1 leaderboard |
| `Achievement001.PNG` … `Achievement012.PNG` | 12 files | Achievement images |
| `banner.png`, `title_icon.png` | 77,204 / 8,084 | Dashboard art |

`ArcadeInfo.xml` also gives `xdkVersion="2.0.8276.0"` and `projectVersion="1.0.244.0"`.

## default.xex

| Field | Value |
|---|---|
| Size | 4,808,704 bytes |
| CRC32 | `FC04059D` |
| SHA-256 | `5FF5B37E120B1F2C3057D78BFC8C8AD6885C3FA576AECB3026E23E6044C5C3DE` |
| SHA-1 | `9D0BFD4DF24DEB79F87E85195D9C9A0663866FA0` |
| MD5 | `4C2E6A8AEFDB88544B09AE2E2A64FBA6` |
| Format | XEX2, module flags `0x00000001` (title) |
| Original PE name | `default.exe` |
| Encryption / compression | Normal (retail AES key) / Normal (LZX, window `0x8000`) |
| Image base / load address | `0x82000000` |
| Image size | `0xC40000` |
| Entry point | `0x821481C0` |
| PE timestamp | `0x4A69351C` (2009-07-24 04:14:20 UTC) |
| Execution info | Title `0x584109D2`, Media `0x23B57DFA`, version and base version 0.0.1.3, disc 0/0 |
| Region / allowed media | `0xFFFFFFFF` / `0x18000000` |
| System flags | `0x204`: `NO_ODD_MAPPING`, `USES_GAME_VOICE_CHANNEL` |
| Default stack size | `0x80000` |
| TLS | 64 slots, no TLS data |
| Resource | `584109D2` (XDBF) at `0x82C10000`, `0x27347` bytes |
| Export table | `0x82A6F630` |

Static libraries (all XDK 2.0.8276.0): D3D9, D3DX9, XGRAPHC, XAPILIB, XBOXKRNL, LIBCMT, XAUDIO2, X3DAUD, XAPOBA,
XMCORE, XMP, XONLINE, XPARTY, XHV2.

Imports: `xam.xex` (174 import records) and `xboxkrnl.exe` (312 records), both version 2.0.8276.0, minimum 2.0.1861.0.
ReXGlue codegen resolved 237 distinct imported functions.

## TMNT_r.xex

| Field | Value |
|---|---|
| Size | 2,002,944 bytes |
| CRC32 | `2D0061F9` |
| SHA-256 | `C6C19BD1D3DE0C0AF3651DE201E8CAC47D9C380C03060734F6F649E83EE89DD9` |
| SHA-1 | `025EF4C5D5A659B4F86CF1180111C352B8421AB5` |
| MD5 | `278501193919B38E8238DCA1019156E4` |
| Format | XEX2, module flags `0x00000009` (title module + DLL) |
| Original PE name | `TMNT_Final_r.dll` |
| Encryption / compression | Normal (retail AES key) / Basic |
| Image base / load address | `0x87500000` |
| Image size | `0x1F0000` |
| Entry point | `0x87520000` |
| PE timestamp | `0x4A69356F` (2009-07-24 04:15:43 UTC) |
| Execution info | Same title, media and version as `default.xex` |
| Export table | `0x876DEE38` |

Static libraries: LIBCMT, XAPILIB, XBOXKRNL (XDK 2.0.8276.0). Imports: `xboxkrnl.exe` only (102 records, 51 distinct
functions resolved by codegen). No Title Update is present in the supplied files.

## API areas used

- **Graphics:** D3D9/D3DX9/XGRAPHC statically linked; `Vd*` kernel calls including `VdSwap`, `VdInitializeRingBuffer`,
  `VdSetDisplayMode`, `VdQueryVideoMode`, `VdRetrainEDRAM`. The shipped `.fx` sources and the `Data/Cache` effect lists
  suggest the engine may handle effects at run time; not verified.
- **Audio:** XAUDIO2, X3DAUD, XAPOBA and XMCORE statically linked; `XAudio*` render-driver calls and
  `XMACreateContext`/`XMAReleaseContext`. XMP (background music) is linked.
- **Input:** `XamInputGetState`, `XamInputSetState`, `XamInputGetCapabilities`.
- **Networking / Xbox Live:** `NetDll_*` sockets, `XNet*` (including QoS and key registration), `XamSession*`,
  `XamUser*`, `XamShow*UI`, `XamVoice*` (XHV2 voice), XONLINE and XPARTY.
- **Filesystem and content:** `NtCreateFile`/`NtReadFile`/`NtReadFileScatter`/`NtQueryDirectoryFile`, `XamContent*`,
  `StfsCreateDevice`/`StfsControlDevice`, `ObCreateSymbolicLink`, and a custom I/O device (`IoCreateDevice`,
  `IoCompleteRequest`, `IoSetShareAccess`, ...).
- **Modules:** `XexLoadImage`, `XexGetProcedureAddress`, `XexUnloadImage`, `XexGetModuleHandle`.

## Identity checks

This is the XBLA release: content type `000D0000`, a `LIVE` package, the expected Title ID in all three sources, and
the same Media ID and version in the package header and both executables.
