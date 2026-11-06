# Addresses

Gothic 2 NotR 2.6 fix, Steam `Gothic2.exe`: 9,038,140 bytes, PE checksum `0x008A3EA9`, ImageBase `0x00400000`.
Source: disassembly of this exe, cross-checked with Ghidra.

## Exe check

FNV-1a fingerprint over all code ranges the plugin uses (`kCodeRanges` in `src/vobway.cpp`), expected value `0xE48C57EE`.
Other 2.6 fix builds are accepted when this code is identical.

## Patched

| Address | What | Original bytes |
|---|---|---|
| `0x00680E6F` | `jne 0x6811B1` in `oCNpc::CanGo`, ray hits a vob | `0F 85 3C 03 00 00` |
| `0x00680DF2` | contact vob without polygon, ends in `return 2` | `D9 46 64 83 EC 0C` |
| `0x00680EE5` | `je 0x6811B1` after `CanWalkOnPoly` on the world polygon hit by the ray (e.g. cliff face behind a vob ramp) | `0F 84 C6 02 00 00` |
| `0x00688157` | call of `RbtGetObstacle` in `RbtCheckLastDirection` (avoid mode) | `E8 84 F1 FF FF` |
| `0x00687633` | call of `CanGo`, only with `Diag=1` | `E8 F8 94 FF FF` |

## Jump targets

| Address | What |
|---|---|
| `0x006811B1` | `return 2`, avoid |
| `0x00680DAD` | path after both rays missed, checks the contact vob |
| `0x00680EF7` | path without contact vob |
| `0x00680DF8` | continuation after the displaced instructions |

## Calls

| Address | What |
|---|---|
| `0x00680B30` | `oCNpc::CanGo`, `ret 8` |
| `0x006B25A0` | `oCAniCtrl_Human::CanWalkOnPoly`, `ret 4` |
| `zCVob` vtable `+0x60` | `TraceRay(start, ray, flags, report)`, `ret 10h`; flag `0x80` returns the normal. Base version `0x5FFE40` clears 0x28 bytes of report; without visual (`+0xC8`) the result is 0. No vob class overrides it. |
| `0x00621FA0` | `zCWorld::TraceRayNearestHit(start, ray, ignoreVob, flags)`: start in `edx`, the rest on the stack, `ret 0Ch`. Report in `zCWorld` from `+0x38`: `+0x3C` vob, `+0x40` polygon, `+0x44` intersection (y at `+0x48`), `+0x50` normal (with flag `0x80`). Flags `0x8A1` as the engine's first ray (`0x6874D5`). |
| `0x006872E0` | `RbtGetObstacle(start, dir, zCVob*& hitVob, zVEC3& normal)`, `ret 10h` |

## Read only, not patched

| Address | What |
|---|---|
| `0x00687490` | first ray from feet height + 10, flags `0x8A1`, result 1 only for `oCMOB` (`ebx` in the stub) |
| `0x006872E0` | second ray from NPC centre, flags `0xA5`, checks candidates via `0x687080` |
| `0x00687080` | `oCNpc::RbtIsObjectObstacle` (name from the exe's debug text), `TraceRay` with flag `0x80` |
| `0x0061E650` | reads `zCVob +0x11C` |
| `0x00687B7C` | reader of `oCNpc +0x4D8`, only evaluates NPC and class `0xAB1518` |
| `0x006ED39F` | reader of `oCNpc +0x4D8` (also `0x6ED6F6`, `0x6ED82C`), `dynamic_cast` to `oCMobInter` |

## Classes

| Address | Class |
|---|---|
| `0x00AB1E20` | `oCNpc::classDef` |
| `0x00AB1A10` | `oCMOB::classDef` |
| `0x00AB1168` | `oCItem::classDef` |
| `0x009A3A60` | `zCMover::classDef` |
| `0x009A35B8` | `zCVobLevelCompo::classDef` |
| `0x009A3900` | `zCVobStair::classDef`, not patched (the engine already skips it) |

## Fields

| Offset | What |
|---|---|
| `oCNpc +0xB8` | `homeWorld` (`zCWorld`) |
| `oCNpc +0xC0` | AI |
| `oCNpc +0x4D8` | ray vob |
| `oCNpc +0x980` | AniCtrl |
| `AniCtrl +0x7C` | feet height (world) |
| `AI +0x4C` | slope limit (radians) |
| `AI +0x64` | vob |
| `zCVob +0x44/54/64` | at vector |
| `zCVob +0x48/58/68` | position |
| `zCVob +0x104` | flags; `0x687080` requires bit 7 and clears bit 5 for the call |
| `zCVob +0xC8` | visual |
| `zCVob +0x11C` | collision object |
| collision object `+0x10C` | contact polygon |
| collision object `+0x110` | contact vob |
| collision object `+0x12D` | flags |
| trace report `+0x10` | hit height (world) |
| trace report `+0x18` | normal (world) |
| `zCClassDef +0x3C` | base class |

## CanGo results in the caller RbtAvoidObstacles

| Result | Meaning |
|---|---|
| 0 | no avoidance |
| 2 | avoid, turn on the spot |
| 1, 3, 4 | own branches (3 jump/climb, 4 turn back at a drop) |
