# VobWay

A Union plugin for **Gothic 2 Night of the Raven 2.6 fix** that lets NPCs on the waynet walk up sloped vob surfaces (ramps, stairs, platforms placed as vobs in the Spacer) instead of turning on the spot in front of them.

## Install

1. Install Union (or a system pack that includes it).
2. Download `VobWay.zip` from the [latest release](../../releases/latest).
3. Copy `VobWay.dll` and `VobWay.ini` into `Gothic II\System\Autorun\`.

That's it. With the shipped `VobWay.ini` nothing needs to be changed.

To uninstall, delete both files.

## Requirements

- Gothic 2 NotR **2.6 fix**. The plugin was built against the Steam `Gothic2.exe` (9,038,140 bytes, PE checksum `0x008A3EA9`).
- At start the plugin compares every piece of engine code it touches with that version. Other 2.6 fix builds (GOG, CD) work if that code is identical. If it does not match, or another patch has already changed the same spots, VobWay changes nothing.
- The waypoints must already lead over the ramp, and the vob needs collision.

## The problem

When an NPC walks along the waynet, the engine checks before each step, with a ray forward and with body contact, whether something is in the way:

- If the ray hits **world mesh**, the engine checks the slope of the surface and lets the NPC walk up if it is flat enough.
- If the ray hits a **vob**, or the body touches one, the vob counts as an obstacle **without any slope check**, and the NPC turns on the spot.

A flat vob floor lies below the ray and does not trigger this, which is why flat vob floors work. A vob ramp is hit by the ray, so NPCs turn in front of it.

There is one more case: if a vob ramp leans against a cliff made of world mesh, the ray can pass the ramp and hit the vertical cliff face behind it. Then the engine makes the NPC turn because of the wall.

## What VobWay does

The goal is that a vob behaves for NPCs as if it were world mesh.

At these engine checks, the plugin probes the ground in front of the NPC from top to bottom, every 20 cm up to 160 cm ahead by default:

1. First only the vob that was hit.
2. Then world mesh and all vobs together, so transitions work: vob ramp onto a world mesh cliff, world mesh ramp onto a vob platform, ramp onto a second vob.

The step is allowed when every surface is flat enough and the ground does not rise from point to point by more than the slope limit plus one step (30 cm by default). Two rays forward, at body centre and at 1 m height, detect walls and railings; by default they are as long as the engine's own ray for humans (130 cm).

Once an NPC has avoided an obstacle, the engine keeps checking whether its original way is free again. The plugin also judges vob ground there, so the NPC finds its way back to the ramp.

These stay with the engine:

- NPCs, mobs, items, movers and level composites stay obstacles.
- Pure world mesh without a vob keeps the engine's decision, and so does the player.
- `zCVobStair` is not touched, because the engine already skips it.

Patched addresses and offsets are listed in [ADDRESSES.md](ADDRESSES.md).

## Results

- **In game:** confirmed working out of the box by the user who asked for the plugin.
- **In a simulation:** the exe's own machine code for these checks and for NPC collision was run in a test program on test meshes. The results below are with default settings:
  - Vob ramps up to 49° are walked to the top; from about 51° they are not (the engine limit is 50°).
  - A 40° vob ramp against a 1.5 m world mesh cliff is walked onto the plateau, even if its top ends 10 cm below the edge.
  - The same shapes as vob and as world mesh behave the same: ramps from 20 to 55°, stairs with 15 to 40 cm steps, an 80 cm step, a 90 cm wall, and a ramp with a 20 cm ledge.
  - Crates from about 60 cm, walls, railings and mobs stay obstacles.

## Settings

All settings are explained in [VobWay.ini](VobWay.ini). Invalid values are ignored and the default applies. The ini may lie next to the DLL, in `Gothic II\System\`, or as a `[VOBWAY]` block in `Gothic.ini`.

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | Plugin on/off. 0 = Gothic runs as without the plugin. |
| `MaxSlope` | 0 | Maximum slope of a vob surface in degrees. 0 = same as world mesh (50° for humans). |
| `CheckRay`, `CheckContact`, `CheckWorld`, `CheckAvoid` | 1 | Turn single check sites on or off, to narrow down a problem. |
| `ProbeStep`, `ProbeReach`, `ProbeUp`, `ProbeDown` | 20, 160, 180, 100 | How densely, how far, how high and how deep the ground is probed (cm). |
| `StepHeight` | 30 | How high an edge between two probe points may be (cm). |
| `WallRayLength`, `WallRayHeight` | 130, 100 | Length and height of the wall rays (cm). 130 = like world mesh; 60 = more generous on stairs. |
| `Diag`, `DiagMaxLines` | 0, 2000 | Write `VobWay.log` and the maximum number of lines. |

Tip: if vob stairs do not work, try `WallRayLength=60`.

## Log and bug reports

With `Diag=1` the plugin writes `VobWay.log` next to the DLL; with `Diag=0` it writes nothing.

1. The first line shows that the DLL was loaded, which exe is running and whether its code matches.
2. The next line is `VobWay v2.1: active` with all settings, or `No patch` with the reason.
3. In game, every decision of the plugin (`walkable=1` = way free) and every refusal by the engine (`CanGo=2 avoid`, `CanGo=3 jump/climb`, `CanGo=4 turn back`) is logged. Each line gives the positions of NPC and vob (x y z, as in the Spacer).

If `VobWay.log` is missing despite `Diag=1`, the DLL was not loaded (usually Union is missing) or the ini was not found.

When reporting a problem, please attach this log and describe the spot (ramp angle, vob or world mesh, where the waypoints are).

A good test is the same vob ramp with waynet, once with `Enabled=0` and once with `Enabled=1`, with `Diag=1` both times.

## Limits

- Stair steps of 40 cm and more stay obstacles with any setting.
- Large creatures were only simulated with estimated model sizes.

## Alternative without the plugin

The engine skips `zCVobStair` in this obstacle check. A ramp placed in the Spacer as `zCVobStair` instead of `zCVob` is therefore walked without the plugin. The downside is that such a vob is never an obstacle for NPCs in this check, not even at its sides. This has not been tested in game.

## Building

You need Visual Studio 2019 or newer with the "Desktop development with C++" workload. Run `build.bat`; the result is `build\VobWay.dll` (32-bit, static runtime). The whole plugin is a single file, [src/vobway.cpp](src/vobway.cpp).

## License

[MIT](LICENSE)
