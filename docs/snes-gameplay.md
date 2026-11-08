# SNES Gameplay

Optional mechanics of the original SNES game, on the launcher's **SNES** page under GAMEPLAY and in the F9 menu. They
need the gameplay data of a Ready SNES ROM import ([classic-soundtrack.md](classic-soundtrack.md)); without it every row
is disabled and the game is stock Re-Shelled. Each row is its own setting, F9 changes it for the session only, and
online each player's choice applies to their own turtle ([online.md](online.md)).

| Row | Setting | Values | Default |
| --- | --- | --- | --- |
| Directional Combat | `tmnt_snes_directional_combat` | `true`, `false` | `true` |
| SNES Throw Control | `tmnt_snes_throw_control` | `true`, `false` | `true` |
| Disable Double Jump | `tmnt_snes_disable_double_jump` | `true`, `false` | `false` |
| SNES Dash | `tmnt_snes_dash` | `"off"`, `"auto"`, `"manual"` | `"auto"` |

## Disable Double Jump

The SNES game has no double jump. With the row On, the first jump works as usual; a jump request made while the turtle
is already in its jump is ignored, so there is no second jump and no jump higher. Landing resets it, and the next first
jump works. Jumps the game itself starts (bounces off a head, scripted jumps) and jump kicks are not touched, and a
turtle on a surfboard or hoverboard keeps its own rules. Off is Re-Shelled's double jump.

## SNES Dash

- **Off** is Re-Shelled's movement: a full push runs at once.
- **Auto** walks until one horizontal direction has been held for 32 SNES frames, then dashes (about 0.53 s on NTSC and
  0.64 s on PAL). A diagonal, a vertical push, a direction change or letting go starts the count again.
- **Manual** walks until a double tap: push the same horizontal direction from the centre, let go, and push it again
  within 16 SNES frames of the first push (about 0.27 s NTSC, 0.32 s PAL). The second push starts the dash, which lasts
  while the direction is held.

Manual does not dash from holding one side, from left then right or right then left, from a vertical or diagonal push, or
without letting go between the pushes. A push that is not a full push (a partly pushed stick) ends the sequence. A late
second push counts as a new first push. A dash needs its own double tap each time. The sequence is kept per controller
and is dropped on a mode change, for a new turtle (stage change, respawn, death) and while the game is paused, so a pause
never completes a double tap.

The windows are SNES frames at the imported ROM's clock (60.0988 Hz NTSC, 50.007 Hz PAL), never the host's refresh. The
dash attack is Re-Shelled's.

A saved `tmnt_snes_dash = true` from before the modes is rewritten to `"auto"` and `false` to `"off"` when the settings
file is read. It never becomes Manual. F9 cycles Off, Auto, Manual with Enter or A; Left and Right step back and forth.

## Online

Support is global and choices are per player. A mechanic is available in a match only when every PC supports it with the
same data identity; then each seat's own choice, including its Dash mode, applies to the turtle that PC controls. The room
fixes one table per gameplay match and every PC uses it whole or not at all.

| Mechanic | Identity (what must match on every PC) | PAL and NTSC |
| --- | --- | --- |
| Directional Combat | the facing table | compatible |
| SNES Throw Control | the facing table, the throw hold in frames, the clock | not compatible |
| Disable Double Jump | the rule version (no ROM data) | compatible |
| SNES Dash | modes Off, Auto and Manual, the hold and tap frames, the clock | not compatible |

On the wire a seat is `dash` (on) and `dash_mode` (`auto` or `manual`); a Dash without a mode is Auto, and a mode this
game does not know is Off. A client that predates the modes has a different Dash identity, so Dash is unavailable between
it and a new client. A service without the `snes_modes` capability never receives Manual (the player's Dash is Off for the
match, and F9 says so), and Disable Double Jump is unavailable. In every such case the match plays
stock rather than risk a desync.

The owner's PC decides its turtle and the game replicates the result. The jump rule is applied only to the turtle each PC
controls.

## Not included

**SNES Special HP Cost.** Re-Shelled has no special attack button: its Y is the Strong Attack, which the port does not
treat as the SNES special, so the SNES cost (Leonardo 10, Michelangelo 12, Donatello 12, Raphael 9, charged when the special
connects and only above that much health) is not applied.
