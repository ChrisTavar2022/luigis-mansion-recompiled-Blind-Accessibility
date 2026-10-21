# Controls (F1 > Controls)

Controller configuration lives in the F1 menu's **Controls** tab
(`src/recomp/controls.cpp`, hooked into `menu.cpp` and `hle_pad.cpp`). Everything applies
live and is saved in `settings.ini` when the menu closes.

## Sub-tabs

**Ports**: what drives each GameCube port (Luigi's Mansion only reads port 1).

- *Auto (next free controller)*: the next connected gamepad that no other port has
  chosen, in connection order. This is the default for all four ports.
- *None*: the port reads as unplugged.
- *Keyboard only*: the keyboard and mouse profile, no gamepad.
- *A specific gamepad*: stays assigned to that controller. When it is unplugged, the port
  shows "Waiting for ..." and picks it up again on reconnect. Controllers of the same
  model, such as the four ports of a GameCube adapter, are told apart by their order.
- *+ Keyboard*: the keyboard and mouse also drive the port, together with its gamepad.
  This is on for port 1 by default.

The status column follows hot-plugging. *Input test* shows a port's live values (buttons,
both sticks, both analog triggers) after bindings and stick shaping, which is what the
game receives before its own `PADClamp`. Keyboard keys reach the game, and this test,
only while the menu window does not have keyboard focus.

**Bindings**: one profile for the keyboard and mouse, and one for each gamepad model (by
SDL GUID). The first time a model is seen, its profile is created from the defaults for
its type. Every GameCube input has a *Primary* and an *Alternate* slot: A, B, X, Y, Z,
START, L and R clicks, the D-pad, the four directions of the control stick and the
C-stick, and analog L/R.

- Click a slot, then press a key or mouse button (keyboard profile), or press a button or
  move a stick or trigger past half way (gamepad profile). Esc or F1 cancels the capture,
  and so do 6 seconds of no input. These keys are never bound, and the key that ends a
  capture does not reach the menu or game hotkeys.
- Right-click a slot to clear it. *Reset bindings to defaults* and *Clear all* apply to
  the shown profile.
- A slot shown in red has an input that is also bound to another GameCube input (the
  tooltip names it). Binding the same trigger to both the L click and analog L, or the R
  click and analog R, is the normal pairing and is not marked.
- Any input can be bound to any GameCube input: a button to a stick direction or an
  analog trigger, or a stick or trigger direction to a button.

**Sticks, triggers & rumble** (gamepad profiles):

- *Dead zone* (control stick and C-stick, radial, 0 to 50%): tilt inside it reads as
  centred, and the rest is rescaled so full tilt is still reachable. The game also applies
  the GameCube's own small dead zone (`PADClamp`, 15/127) afterwards.
- *Sensitivity* (0.5x to 2x): multiplies the tilt after the dead zone.
- *Invert X / Invert Y* for each stick.
- *L / R click at*: how far an analog trigger bound to the L or R click must be pulled to
  press it (default 85%). A pressed L/R click also reads as a full analog press (180),
  like a GameCube trigger at its click.
- *Rumble* on or off, and *Strength* for controllers that support rumble intensity. A
  GameCube controller's motor is on/off only. *Test rumble* runs it for 0.6 s.

The game reads analog L and R as well as the click bits. Its generic pad axis reader
`fn_800807EC` returns `JUTGamePad` analog L/R (pad +0x26/+0x27, the `PADClamp`ed 0..150
range normalized), and other code reads pad +0x27 directly. So partial trigger pulls
matter, and analog triggers are bound by default.

## Default layouts

Keyboard and mouse:

| GameCube | Keys |
|---|---|
| Control stick | W A S D |
| C-stick | Arrow keys |
| A | E |
| B | Q |
| X | R |
| Y | F |
| Z | Left Ctrl, C |
| START | Enter |
| L | Left Shift, right mouse button |
| R | Space, left mouse button |
| D-pad up / down / left / right | 1 / 2 / 3 / 4 |
| Analog L / R | (none: the L/R clicks give a full press) |

Tab (fast-forward), F1 (menu), F3/F4 (debug overlays) and Esc are left free.

Gamepads (face buttons by position, except that Nintendo pads use their printed letters):

| GameCube | Xbox | PlayStation | Switch Pro / Joy-Con pair | GameCube controller (adapter / NSO) |
|---|---|---|---|---|
| A | A | Cross | A | A |
| B | X | Square | B | B |
| X | B | Circle | X | X |
| Y | Y | Triangle | Y | Y |
| Z | RB, LB | R1, L1 | R, L | Z |
| START | Menu, View | Options, Share | +, - | START |
| L / R click | LT / RT past 85% | L2 / R2 past 85% | ZL / ZR | L / R click |
| Analog L / R | LT / RT | L2 / R2 | ZL / ZR | L / R |
| Control stick | Left stick | Left stick | Left stick | Control stick |
| C-stick | Right stick | Right stick | Right stick | C-stick |
| D-pad | D-pad | D-pad | D-pad | D-pad |

Gamepads default to a 8% stick dead zone. A GameCube controller defaults to 0%, because
the game's `PADClamp` already provides the original dead zone. Rumble is on at 50%
strength, Aurora's default.

## settings.ini

```ini
[controls]
port1_device=auto              ; auto | none | keyboard | pad:<SDL GUID>#<n>
port1_device_name=...          ; for a chosen pad (shown while it is unplugged)
port1_keyboard=true
...
[controls.keyboard]
a=key:E                        ; key:<SDL scancode name> | mouse:<1-5>
a.2=                           ; alternate slot; empty = unbound
...
[controls.pad.<SDL GUID>]      ; only profiles that differ from their defaults
name=Xbox Series X Controller
type=3                         ; SDL_GamepadType; rebuilds the defaults when it comes first
a=button:south                 ; button:<south|east|west|north|leftshoulder|misc3|...>
l=axis:lefttrigger+            ; axis:<leftx|lefty|rightx|righty|lefttrigger|righttrigger><+|->
stick_deadzone=0.08
c_deadzone=0.08
stick_sensitivity=1
c_sensitivity=1
stick_invert_x=false
stick_invert_y=false
c_invert_x=false
c_invert_y=false
l_threshold=0.85
r_threshold=0.85
rumble=true
rumble_strength=0.5
```

Input keys: `a b x y z start l r dpad_up dpad_down dpad_left dpad_right stick_up
stick_down stick_left stick_right c_up c_down c_left c_right analog_l analog_r`, with `.2`
for the alternate slot. Keys left out keep their defaults. SDL axis directions are raw:
`lefty-` is up.

The ini sections are written through `port::addSettingsExtension` (`settings.h`), which
lets a module own its own sections of `settings.ini`.

## How it drives Aurora

- **Ports**: Aurora's PAD (`lib/dolphin/pad`) gives a port the gamepad whose SDL *player
  index* matches it. `controls::beforeRead` (before every `PADRead`) reassigns player
  indices whenever the connected pads or the settings change. Aurora's own port
  preferences (`controller_ports.dat`) are not used.
- **Bindings**: for each port, Aurora's per-controller button/axis mappings and keyboard
  bindings are emptied, and the profile bindings are handed to
  `aurora::pad::set_action_bindings` as `aurora::binding::Binding`s targeting the PAD
  controls (`aurora::pad::controls()`). This accepts any key, mouse button, gamepad
  button or axis direction for any button, stick axis or trigger. The changes are made in
  memory only: `PADSerializeMappings` is never called, so Aurora's `.controller` and
  `keyboard_bindings.dat` files are neither written nor needed.
- **Shaping**: the radial dead zone and sensitivity are applied to the `PADStatus` after
  `PADRead` (`controls::afterRead`). Inversion is a negative binding scale.
- **Rumble**: see the next section.
- **Capture**: `aurora::binding::capture_next` (a layer above ImGui), with
  `controls::handleEvent` hiding captured keys from `menu::handleEvent`'s F1/Esc and the
  debug hotkeys. While the menu is open, the game still gets neutral input (`hle_pad.cpp`).

## Rumble

The whole path, game to controller (issue #12, "no vibration with an Xbox Series X
controller on Windows 10"):

1. **Game.** The game's Options "Vibration" setting is on by default (a fresh save turns
   the motor on). It calls `JUTGamePad::CRumble::setEnable` with `0xF0000000` when it is on
   (`fn_800557F0`, option byte `0x803CFAA0 + 1`), 0 when off. Rumble events (`fn_800869F4`:
   event codes 42 start, 45 stop, 47 hard stop) call `CRumble::startMotor` /
   `stopMotor` / `stopMotorHard`, and `CRumble::update` (from `JUTGamePad::update`) plays
   bit patterns. Each is one `PADControlMotor(chan, 1|0|2)` per change, and only when the
   port is enabled. The game never calls `PADControlAllMotors`. Luigi's hits: on for about
   1 s, off, on again briefly, then a hard stop. Ghost fights: pulses of 10 retraces on and
   2 off.
2. **HLE.** `HLE(PADControlMotor)` (`hle_pad.cpp`) calls `controls::controlMotor`.
3. **Controls layer.** `controlMotor` sends the motor state to the gamepad that
   `controls::assign` put on that port (by SDL joystick ID, not via the player-index lookup).
   Profile *Rumble* (default on) and *Strength* (default 50%) apply. A GameCube controller
   (adapter) goes through Aurora's on/off path. Any other pad gets `SDL_RumbleGamepad(low =
   high = strength * 65535, 500 ms)`, re-sent every 100 ms while the motor is on (every 25 ms
   after a rejected request), and `SDL_RumbleGamepad(0, 0, 0)` when the motor stops.
4. **SDL.** With a non-zero duration, SDL stops the rumble itself when it runs out and
   re-sends it to the driver every 2 s (`SDL_RUMBLE_RESEND_MS`). A repeated request with the
   same values only extends the expiry.

Before this, rumble went through Aurora's `PADControlMotor`, which calls
`SDL_RumbleGamepad(..., duration 0)`. In SDL 3.4, duration 0 means "until changed": SDL
sets no expiry and clears `rumble_resend`. So the request was sent once. If the backend
rejected it (for example Windows raw input before the pad is correlated with its XInput
slot: "Controller isn't correlated yet"), or the controller stopped on its own (pads that
need SDL's resend), nothing re-sent it until the game changed the motor state. It also
depended on Aurora resolving the port's controller from the SDL player index at that
moment. Without hardware it is not confirmed which of these broke the user's Xbox pad. In
SDL 3.4 on Windows, Xbox pads use the XInput backend by default (raw input, WGI, GameInput
and HIDAPI Xbox are off), and XInput rumble persists, so the logging below exists to
confirm it from a user log.

Diagnostics, always logged: `[controls] port 1: <name> (..., vid/pid, wired|wireless,
rumble yes|not reported)` and `path <SDL device path>` (shows the backend: `XInput#0`,
`\\?\HID#...`). A rejected rumble request is logged once per failure streak with SDL's
reason. F1 > Controls > *Sticks, triggers & rumble* > *Test rumble* vibrates for 0.6 s and
shows "Sent to the controller." or "The controller refused rumble: <SDL error>" beside it.

`LM_RUMBLE_LOG=1` logs every game call (`[rumble] game PADControlMotor(chan 0, cmd 1) from
801D7D08`) and every request sent (`[rumble] port 1 rumble -> '<pad>' low=high=32768 for
500 ms (request n, ...): ok`).

Verified with the virtual pad (no hardware): Luigi's hits (`LM_TEST_HIT=2600,200`), a gold
ghost fight (`LM_GHOST_HUNT`, docs/RENDERING.md), and the menu's Test rumble
(`LM_MENU_CLICK="300:open=Controls/Analog;330:Test rumble;420:close"`) all reach the
virtual pad's rumble callback with 32768/32768 and stop with 0/0. With
`LM_VIRTUAL_PAD_RUMBLE_FAIL=3` the first three requests are rejected and the refresh gets
the rumble through 25 ms later.

## Test aids

- `LM_RUMBLE_LOG=1`: the rumble log above. `LM_VIRTUAL_PAD_RUMBLE_FAIL=<n>`: the virtual
  pad rejects its first n non-zero rumble requests.
- `LM_PAD_LOG=1`: logs every change of each port's status as the game receives it
  (`[pad] port 1 retrace N: err 0 buttons 0100 stick 0,127 C 0,0 L 0 R 180`), the
  port/device assignments, every SDL joystick (and whether it has a gamepad mapping), and
  each assigned pad's SDL mapping string.
- `LM_VIRTUAL_PAD="40:attach,100:south=1,105:south=0,170:lefttrigger=0.9,195:leftx=-1,270:motor=1,280:motor=0,300:detach"`:
  a scripted SDL virtual Xbox 360 pad, timed in `PADRead` calls. It uses raw axis values
  (triggers go from -1 released to 1), and `motor=1|0` calls the game's rumble path on
  port 1. Its rumble output is logged (`virtual pad rumble low .. high ..`). This tests
  assignment, hot-plug, default and custom bindings, shaping and rumble without real
  hardware.
- Scripted input (`--input "600:START*10,1000:A*6"`, `<frame>:<buttons>[*<frames>]`, buttons
  joined with `+`): items prefixed `P2:` (or `P3:`/`P4:`) drive that port instead of port 1,
  e.g. `--input "1000:A,P2:2600:STICKLEFT*200,P2:3000:R*60"`. A port the script uses reads as
  connected for the whole run, so the game sees a pad plugged in (used for the 2-player
  mode, docs/UNUSED_CONTENT.md). With the 2-player "Co-op improvements" the game ignores port 2's
  X, Y and D-pad (player 2 has no Game Boy Horror) and all of port 2 while an event has set
  player 2 aside (hle_pad.cpp).
- `LM_MENU=Controls` opens the menu on this tab. `LM_CONTROLS_TAB=Ports|Bindings|Analog`
  selects a sub-tab (also `LM_MENU_CLICK` `open=Controls/Analog`).
- `LM_CONTROLS_CAPTURE=<keyboard|pad>:<input key>:<1|2>` starts a binding capture when the
  tab opens. This capture has no timeout. For example, `keyboard:a:1` and then pressing K
  binds A to K.

Verified this way: every default keyboard binding (real key events into the window), the
default Xbox layout, a custom pad profile from `settings.ini` (rebinding, unbinding,
inverted Y, 2x sensitivity, rumble off), the dead zone rescale, the trigger click
threshold, hot-plug in and out, F1 during a capture (it cancels and the menu stays open),
and saving to `settings.ini`.

## Known issues

- The Switch 2 NSO GameCube controller (USB 057E:2073) is not always recognised by SDL
  3.4.10 on Windows. On some runs, SDL's HIDAPI driver handles it ("Nintendo GameCube
  Controller", GameCube layout, L/R clicks on `misc3`/`misc4`). On other runs, opening its
  HID interface through libusb fails, and it is only a raw joystick "If_Hid" without a
  gamepad mapping, which Aurora does not use. `LM_PAD_LOG=1` shows which happened. The
  official GameCube adapter (057E:0337) needs the WinUSB driver (Zadig) for SDL to see it.
- Keyboard input is ignored by the game while the F1 window has keyboard focus. This is
  intended: the menu owns input.
