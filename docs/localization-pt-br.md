# Português Brasil (pt-BR) localization

Português Brasil is an **unofficial localization added by this PC port**. The original Xbox 360 release has no Brazilian
Portuguese, and this is not a Ubisoft localization. The launcher lists English, French, German, Italian, Spanish and
**Português Brasil**. The first five are the game's own languages and are unchanged.

Nothing on disk changes: the game files stay as they are, and no modified XEX or archive is produced. Everything is applied
in memory while the game runs, from the player's own files.

## Language mapping

| | |
| --- | --- |
| Setting | `tmnt_language = "pt-BR"` in `settings.toml` |
| Port language | Português Brasil, reported as `Language=Português Brasil` in the diagnostics |
| Guest language slot | English (`user_language` 1); the game has no Portuguese bank, and an unknown Xbox language ID is never invented |
| Runtime state | `tmnt::localization::SetActiveLanguage`, set at start and every launcher frame |

An unknown `tmnt_language` value falls back to English, as before.

## Architecture

| Text | Where it lives | How pt-BR is applied |
| --- | --- | --- |
| Menus, HUD, errors, wait messages, subtitles | `UI_TextBank`, `UI_ErrorTextBank`, `UI_WaitingTextBank`, `UI_MovieSubtitleBank` `.txl` banks in `Data\TMNT.bf`, one per language | `src/localization/game_text_hook.cpp` hooks the bank parser `sub_82291888` and rebuilds the English bank's pools |
| Font | Glyph descriptor and DXT5 atlas in the `f88d*.bin` bundles, a `DefineFont3` code table in each of the 55 movies | `src/localization/font_patch.cpp`, called from the bundle hook `src/fire_menu_hook.cpp` |
| Launcher, online page, dialogs, overlays | String literals in `src/` | `tmnt::Tr()` looks the English text up in the pt-BR table; TRG draws through the same lookup (`patches/trg-launcher/0006-ui-translator.patch`) |
| Run-time messages (`<name> invited you to a party.`, `In your party · Connected · Direct · 12 ms`) | Built by the online layer in English | `tmnt::TrMessage()` replaces known phrases at the point of display; protocol and service code never see a translation |
| Achievements | XDBF in `default.xex`, one set per language | pt-BR rows of the UI table, applied in the launcher, the F7 overlay and `src/achievement_toast.cpp` |
| Subtitle default | `Localization_GetDefaultSubtitleSettings` turns subtitles off for English | `src/localization/subtitle_default_tmnt_r.cpp` turns them on under pt-BR, because the speech stays in English |

### How the game reads text

Scripts call `Localization_Get_Text(rank)` and its Error, Wait and Subtitle siblings, which resolve to `TXT_GetByRank` and
`TXT_cwstr`. The engine loads one `.txl` per bank for the language index. A loaded bank has records of 28 bytes (id at 0,
character offset at 4), a UTF-16 pool and a byte pool: every unit truncated to 8 bits. Scripts pass the byte form to the
movies, so **game text is Latin-1**: nothing above U+00FF can be displayed.

Movies ask for their strings with `UI_FIRE_REC_LOC_STR_GET`. Strings go through `SCR_StringCpy(buffer, text, 200)`, so a
string must stay under 200 bytes.

Under pt-BR the hook runs after the parser, builds new pools from the table with the engine's own heap (the unload frees
them there) and writes the new offsets into the records.

### Font

The movies' font, `TMNTredux`, is drawn from a pre-rendered atlas, not from its outlines. `TextBuffer` looks each character
up in the descriptor (178 glyphs at offset 72 of `f88d0001.bin`), which points into a 512x512 DXT5 atlas. The atlas is
stored bottom-up in every bundle, so a glyph's row y is atlas row 511 - y.

The font has no `ã õ Ã Õ`. `font_patch.cpp` adds them under pt-BR:

* Each is drawn from the player's own atlas: the circumflex of `â ô Â Ô` is replaced by the tilde of `ñ Ñ`. No game data is
  stored in this repository.
* They take the slots of `ä ö Ä Ö`, the neighbours in the sorted code tables, so no table is reordered and no size changes.
  Portuguese and every credit name use none of the four. Other languages are not affected: the patch runs only under pt-BR.
* The glyphs go to free cells of the atlas from x = 64, y = 4; all other blocks stay as shipped.
* Every step checks the supported bytes first (a hash of the descriptor and the atlas). Anything else is left unchanged.

## Source of truth

| File | Rows |
| --- | --- |
| `src/localization/pt_br_game_text.cpp` | `bank<TAB>rank<TAB>name<TAB>crc32 of the English text<TAB>text` |
| `src/localization/pt_br_ui_text.cpp` | `english<TAB>text` |

A `=` text keeps the English on purpose. The CRC pins a row to its English source: a row that no longer matches stays English
and is counted as stale in the log. Escapes in text: `\r`, `\n`, `\t`, `\\`.

## Rules

* Brazilian Portuguese vocabulary, natural game-UI register, no literal word order.
* Game text: Latin-1 only, at most 199 bytes, and `\r`, `%s` and `%d` as in the English row.
* Keep a label no longer than the longest shipped language. Seven rows are longer, because the glossary calls for them; the
  settings fields were measured: `SELECIONAR`, `EFEITOS SONOROS`, `NÚMERO DE JOGADORES`, `VELOCIDADE`, `LIGADO`, `DESLIGADO`.
* Character names stay as they are. So do FPS, Ping, UDP, NAT, STUN, Relay, VSync, MSAA, Direct3D, Xbox, XUID and gamertag.

## Glossary

| English | pt-BR |
| --- | --- |
| Single Player / Multiplayer | Um Jogador / Multijogador |
| Help & Options | Ajuda e Opções |
| Achievements | Conquistas |
| Exit Game / Resume Game | Sair do Jogo / Continuar Jogo |
| Settings / Controls / How to Play / Credits | Configurações / Controles / Como Jogar / Créditos |
| Language / Display / Graphics / Gameplay | Idioma / Tela / Gráficos / Jogabilidade |
| Player / Players | Jogador / Jogadores |
| Create Match / Quick Match / Custom Match / Find Match | Criar Partida / Partida Rápida / Partida Personalizada / Buscar Partida |
| Invite Friends / Lobby / Ready | Convidar Amigos / Lobby / Pronto |
| Back / Select / Continue / Search | Voltar / Selecionar / Continuar / Buscar |
| Difficulty: Easy, Normal, Hard, Hardcore | Dificuldade: Fácil, Normal, Difícil, Hardcore |
| Story / Survival / Quickplay | História / Sobrevivência / Jogo Rápido |
| Warning / Error / Yes / No | Aviso / Erro / Sim / Não |
| Music / Sound FX / Brightness / Subtitles | Música / Efeitos Sonoros / Brilho / Legendas |
| Default / On / Off | Padrão / Ligado / Desligado |
| Connection / Connection Test / Direct / Relay | Conexão / Teste de Conexão / Direto / Relay |
| Party / Host / Level | Grupo / Anfitrião / Fase |
| Leaderboards / Score | Placares / Pontos |

## Text that stays English

Kept with `=` in the game table, 80 rows:

* **Credits:** every role and name. All shipped languages keep the English roles; the rows are layout-sensitive.
* **Stage titles:** `BIG APPLE, 3 AM` and the rest. All shipped languages keep them.
* **Same word in both languages:** `LOCAL`, `NORMAL`, `HARDCORE`, `STATUS`, `MENU`, `LOBBY`, `VOLUME`, `VERSUS`, `POWER-UPS`, `PING`.
* **Canonical:** `GAMERTAG`, `Turtle Power!`, `Cowabunga!`. Empty and unused rows.

Other text:

* **Logs, crash reports, diagnostics and `DEBUG_SAULO`**, the connection test's copied diagnostics, Worker and admin pages.
* **Key names** other than Space, Enter, Esc and the arrows.
* **The Linux Vulkan error screen** drawn before the launcher: its font is ASCII only.
* **`ä ö Ä Ö`** cannot appear in a pt-BR session; no pt-BR or credit text uses them.

### Engine text calls

`TMNT_r.xex` also holds direct `STR_Display` calls, outside the banks. `localization_tests` pins all 23 and fails on a new one.
None reaches a player:

* `TitleScreen`, `TitleScreenV2`, `PauseMenu2`: pre-FIRE prototypes. No world instantiates their models: their keys are in no
  bundle of the archive, while those of `FireCommunicator.mdl` and `InGameHUD.mdl` are.
* `InGameHUD`: `Are you sure you want to quit?` and `B:Yes A:No` need `m_IsQuitting`, which only its init writes, to false.
* Debug readouts, the cheat menu, `WinWorld`, `CPGCharSel`, `AIStupid`, `DebugDisplay`, `FontScript`.

### Images

The movies hold no bitmaps: they reference 202 textures in the bundles. A contact sheet of all of them shows no words other than
logos (Turtles in Time, Bink, PhysX), so there is no baked menu text to redraw.

## Testing

`tools/test.ps1` runs `tests/localization_tests.cpp` (target `tmnt_localization_tests`):

* Source only: registration, the live launcher sequence, `Tr` and `TrMessage`, table format, duplicates, UTF-8, Latin-1, the guest
  buffer, placeholders, leftover English, hook gating, CMake and gate wiring, the DXT5 codec and glyph composition.
* With `game/extracted`: every row against the real English bank (CRC, markup, length), the font patch on all three bundles, every
  character used against the descriptor and every movie font. Without the data these print `SKIP` and the rest still runs.

Rendering, layout and the runtime paths are checked by hand in the game.
