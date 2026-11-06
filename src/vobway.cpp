// VobWay v2.1 — Union plugin for Gothic 2 NotR 2.6 fix
// License: MIT, see LICENSE.
//
// Addresses from the Steam exe (PE checksum 0x008A3EA9); any build with identical
// code at kCodeRanges is accepted.
// oCNpc::CanGo returns 2 when the NPC must avoid, and the only caller,
// oCNpc::RbtAvoidObstacles, turns it on the spot. Result 0 means no avoidance.
// World polygons get a slope test (oCAniCtrl_Human::CanWalkOnPoly). Vobs do not:
// a ray hit (jne at 0x680E6F) and a body contact without polygon (0x680DF2) both
// end in "return 2". A flat vob triggers neither, a ramp does. A third case: the
// rays pass a vob ramp and hit a steep world polygon behind it (a cliff face the
// ramp leans against); CanWalkOnPoly refuses it at 0x680EE5.
//
// This DLL redirects those three sites, plus the obstacle check of the avoid mode
// (RbtCheckLastDirection at 0x688157) so an NPC that once turned away finds back to
// the ramp. With Diag=1 it also wraps the CanGo call (0x687633) for the log. It probes the vob straight down ahead of
// the NPC and walks the profile from the feet: every hit must be walkable and each
// segment must climb no more than the slope limit plus a step, so a crate top or
// a lip stays an obstacle. Rays at centre and chest height reject walls. At the
// world site it probes world mesh and vobs together and only overrides the engine
// when a placed vob carries the NPC; pure world mesh keeps the engine result.
// NPCs, mobs, items, movers and level composites are never probed. zCVobStair is
// already skipped by the engine and is not touched. The player does not use
// CanGo. FindFloorWaterCeiling is intentionally not hooked.
// Checked against the exe (also with Ghidra) and in a simulation of the engine code;
// confirmed working in game by a user (2026-11).
//

#define VOBWAY_VERSION "v2.1"  // the only place the version is set

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t kImageBase = 0x00400000;

// Patched sites and jump targets inside oCNpc::CanGo.
constexpr uint32_t kVobHitJne = 0x00680E6F;      // jne 0x6811B1 inside oCNpc::CanGo
constexpr uint32_t kContactBlock = 0x00680DF2;   // contact vob without polygon, ends in return 2
constexpr uint32_t kContactResume = 0x00680DF8;  // after the two displaced instructions
constexpr uint32_t kNoContact = 0x00680EF7;      // same path as "no contact vob"
constexpr uint32_t kReturnObstacle = 0x006811B1; // mov eax, 2
constexpr uint32_t kRayClear = 0x00680DAD;       // path after both rays missed, checks the contact vob
constexpr uint32_t kWorldPolyJe = 0x00680EE5;    // je 0x6811B1 after CanWalkOnPoly on the hit world polygon
// zCWorld::TraceRayNearestHit(start, ray, ignoreVob, flags): start in edx, the rest on the
// stack, ret 0Ch (call at 0x6874EC). With an ignore vob it adds it to the list at
// world+0xAC and forwards to 0x621B80 (0x621FCC..0x6220C2).
constexpr uint32_t kWorldTrace = 0x00621FA0;
constexpr uint32_t kCanWalkOnPoly = 0x006B25A0;
constexpr uint32_t kCanGo = 0x00680B30;
constexpr uint32_t kCanGoCall = 0x00687633;      // only call, in RbtAvoidObstacles
// In avoid mode (oCNpc +0x508 > 0) RbtCheckLastDirection traces the stored original direction
// with RbtGetObstacle; any hit keeps the NPC on its detour, so a vob ramp there blocks the way back.
constexpr uint32_t kLastDirCall = 0x00688157;    // call RbtGetObstacle inside RbtCheckLastDirection
constexpr uint32_t kRbtGetObstacle = 0x006872E0; // (start, dir, zCVob*& hitVob, zVEC3& normal), ret 10h

// Class definitions.
constexpr uint32_t kClassNpc = 0x00AB1E20;
constexpr uint32_t kClassMob = 0x00AB1A10;       // oCMOB
constexpr uint32_t kClassItem = 0x00AB1168;      // oCItem
constexpr uint32_t kClassMover = 0x009A3A60;     // zCMover
constexpr uint32_t kClassLevelCompo = 0x009A35B8;

// Member offsets.
constexpr uint32_t kNpcWorld = 0xB8;             // homeWorld, read at 0x687497
constexpr uint32_t kNpcAi = 0xC0;
constexpr uint32_t kNpcTraceVob = 0x4D8;
constexpr uint32_t kNpcAniCtrl = 0x980;
constexpr uint32_t kAniCtrlFeetY = 0x7C;         // world height, trace start at 0x6874CE
constexpr uint32_t kAiSlopeLimit = 0x4C;         // radians, read by CanWalkOnPoly
constexpr uint32_t kAiVob = 0x64;
constexpr uint32_t kVobAtX = 0x44;
constexpr uint32_t kVobPosX = 0x48;
constexpr uint32_t kVobPosY = 0x58;
constexpr uint32_t kVobAtZ = 0x64;
constexpr uint32_t kVobPosZ = 0x68;
constexpr uint32_t kVobCollision = 0x11C;
constexpr uint32_t kVobTraceRaySlot = 0x60 / 4;
constexpr uint32_t kCollFoundPoly = 0x10C;
constexpr uint32_t kCollFoundVob = 0x110;
constexpr uint32_t kCollFlags = 0x12D;
constexpr uint32_t kClassBase = 0x3C;

constexpr int kTraceWantNormal = 0x80;
constexpr uint32_t kReportHitY = 0x10;
constexpr uint32_t kReportNormal = 0x18;
// zCWorld keeps its trace report at +0x38 (cleared at 0x621BD7): vob, polygon, intersection, normal.
constexpr uint32_t kWorldReportVob = 0x3C;
constexpr uint32_t kWorldReportHitY = 0x48;
constexpr uint32_t kWorldReportNormal = 0x50;
// Same flags as the engine's first ray (0x6874D5): only vobs with collision, polygon normal, characters ignored.
constexpr int kWorldTraceFlags = 0x8A1;

// Game units are centimetres. Defaults of the ini settings, all tested in the harness.
// The engine ray is 100 + half the model depth long, so the probes reach 160.
constexpr float kDefaultProbeStep = 20.f;
constexpr float kDefaultProbeReach = 160.f;
constexpr int kMaxProbes = 80;  // ProbeReach 400 / ProbeStep 5
constexpr float kDefaultProbeUp = 180.f;
constexpr float kDefaultProbeDown = 100.f;
constexpr float kDefaultStepHeight = 30.f;
// The second wall ray starts this high above the feet; the first one always at the NPC
// centre, where the engine's second ray starts. No knee-height ray: it hits the risers
// of a stair mesh and refuses it.
constexpr float kDefaultWallRayHeight = 100.f;
// About the engine's own ray length (100 + half the model depth), so vob stairs, edges
// and ramps behave like the same shape in world mesh. 60 is more generous: it reaches the
// risers of a 25..30 cm stair only when the NPC is already on it, so such stairs are climbed.
constexpr float kDefaultWallRayLength = 130.f;
constexpr float kMaxSlopeLimitRad = 1.5f;

constexpr int kDefaultDiagLines = 2000;
constexpr float kPi = 3.14159265358979323846f;

using CanGoFn = int(__thiscall*)(void* self, uint32_t distance, float* normal);
using GetObstacleFn = int(__thiscall*)(void* self, const float* start, const float* dir, void** hitVob, float* normal);
using CanWalkFn = int(__thiscall*)(void* self, const float* normal);
using VobTraceRayFn = int(__thiscall*)(void* self, const float* start, const float* ray, int flags, void* report);

HMODULE g_module = nullptr;
int g_enabled = 1;
int g_diag = 0;
int g_diagLines = 0;
float g_maxSlopeRad = 0.f;
// Ini settings, see VobWay.ini for the explanation of each key.
int g_checkRay = 1;
int g_checkContact = 1;
int g_checkWorld = 1;
int g_checkAvoid = 1;
float g_stepAllowance = kDefaultStepHeight;
float g_probeStep = kDefaultProbeStep;
float g_probeReach = kDefaultProbeReach;
float g_probeUp = kDefaultProbeUp;
float g_probeDown = kDefaultProbeDown;
float g_wallRayLength = kDefaultWallRayLength;
float g_wallRayHeight = kDefaultWallRayHeight;
int g_maxDiagLines = kDefaultDiagLines;
int g_patched = 0;
int g_diagPatched = 0;
uint8_t g_jneSaved[6] = {};
uint8_t g_contactSaved[6] = {};
uint8_t g_worldSaved[6] = {};
uint8_t g_callSaved[5] = {};
uint8_t g_lastDirSaved[5] = {};
int g_lastDirPatched = 0;

uint32_t g_toObstacle = kReturnObstacle;
uint32_t g_toRayClear = kRayClear;
uint32_t g_toNoContact = kNoContact;
uint32_t g_toContactResume = kContactResume;

void* g_lastNpc = nullptr;
int g_lastResult = -1;
void* g_lastDiagNpc = nullptr;
void* g_lastVob = nullptr;
int g_lastWalk = -1;

// Writes only with Diag=1 in the ini; without it the plugin leaves no file behind.
void Log(const char* text) {
    if (!g_diag) return;
    wchar_t path[MAX_PATH];
    if (!g_module || !GetModuleFileNameW(g_module, path, MAX_PATH)) {
        OutputDebugStringA(text);
        OutputDebugStringA("\n");
        return;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"VobWay.log");
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"a") != 0 || !file) return;
    fputs(text, file);
    fputc('\n', file);
    fclose(file);
}

bool Readable(const void* ptr, size_t bytes) {
    if (!ptr) return false;
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(ptr, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT) return false;
    if (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const auto start = reinterpret_cast<uintptr_t>(ptr);
    const auto region = reinterpret_cast<uintptr_t>(info.BaseAddress);
    return start + bytes <= region + info.RegionSize;
}

uint32_t ModuleBase() {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
}

// Every engine byte the patch relies on: CanGo, its two traces and the obstacle test,
// the caller, CanWalkOnPoly, zCVob::TraceRay, the collision getter and one reference
// to each class definition. Other 2.6 fix builds (Steam, GOG, CD) are accepted when
// these bytes are identical, whatever their file size or PE checksum.
struct CodeRange {
    uint32_t begin;
    uint32_t end;
};
constexpr CodeRange kCodeRanges[] = {
    {0x00680B30, 0x00681028}, {0x006811B1, 0x006811C0}, {0x006B25A0, 0x006B2604},
    {0x00687080, 0x006872E0}, {0x00687490, 0x006875DE}, {0x006875E0, 0x00687640},
    {0x005FFE40, 0x005FFE94}, {0x0061E650, 0x0061E657}, {0x00486010, 0x00486015},
    {0x00610F00, 0x00610F05}, {0x005FDC30, 0x005FDC35}, {0x00621B80, 0x00621E70},
    {0x00621FA0, 0x006220C2}, {0x00687F60, 0x00688200},
};
constexpr uint32_t kExpectedFingerprint = 0xE48C57EE;  // Steam exe, PE checksum 0x008A3EA9

// FNV-1a over kCodeRanges. image points at the byte of virtual address kImageBase.
uint32_t CodeFingerprint(const uint8_t* image) {
    uint32_t hash = 2166136261u;
    for (const CodeRange& range : kCodeRanges) {
        for (uint32_t va = range.begin; va < range.end; ++va) {
            hash = (hash ^ image[va - kImageBase]) * 16777619u;
        }
    }
    return hash;
}

uint32_t ExeFileSize() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return 0;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    const DWORD size = GetFileSize(file, nullptr);
    CloseHandle(file);
    return size == INVALID_FILE_SIZE ? 0 : size;
}

// Logs which exe is running, so a bug report always says it.
bool SameCode() {
    char line[256];
    if (ModuleBase() != kImageBase) {
        sprintf_s(line, "VobWay " VOBWAY_VERSION " loaded. Exe is not at 0x400000, so it is not Gothic2.exe. No patch.");
        Log(line);
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(kImageBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(kImageBase + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
        Log("VobWay " VOBWAY_VERSION " loaded. Exe is not a 32-bit program. No patch.");
        return false;
    }
    uint32_t fingerprint = 0;
    __try {
        fingerprint = CodeFingerprint(reinterpret_cast<const uint8_t*>(kImageBase));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fingerprint = 0;
    }
    const bool same = fingerprint == kExpectedFingerprint;
    sprintf_s(line, "VobWay " VOBWAY_VERSION " loaded. Gothic2.exe %lu bytes, PE checksum 0x%08lX, code fingerprint 0x%08lX (expected 0x%08lX)%s",
        static_cast<unsigned long>(ExeFileSize()), static_cast<unsigned long>(nt->OptionalHeader.CheckSum),
        static_cast<unsigned long>(fingerprint), static_cast<unsigned long>(kExpectedFingerprint),
        same ? "." : ". Code does not match 2.6 fix or was already changed by another patch. No patch.");
    Log(line);
    return same;
}

void SkipLine(const char*& p) {
    while (*p && *p != '\n' && *p != '\r') ++p;
    if (*p == '\r') ++p;
    if (*p == '\n') ++p;
}

void Trim(const char*& begin, const char*& end) {
    while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
}

bool KeyIs(const char* begin, const char* end, const char* key) {
    const size_t n = strlen(key);
    if (static_cast<size_t>(end - begin) != n) return false;
    return _strnicmp(begin, key, n) == 0;
}

bool EqualsText(const char* value, const char* literal) {
    return _stricmp(value, literal) == 0;
}

bool IsOff(const char* value) {
    return EqualsText(value, "0") || EqualsText(value, "false") || EqualsText(value, "off") || EqualsText(value, "aus") || EqualsText(value, "nein") ||
        EqualsText(value, "no");
}

bool IsOn(const char* value) {
    return EqualsText(value, "1") || EqualsText(value, "true") || EqualsText(value, "on") || EqualsText(value, "an") || EqualsText(value, "ja") ||
        EqualsText(value, "yes");
}

// On/off values; anything else keeps the previous value.
void ReadSwitch(const char* value, int& target) {
    if (IsOff(value)) {
        target = 0;
    } else if (IsOn(value)) {
        target = 1;
    }
}

// Accepts a decimal comma; values outside [lo, hi] keep the default.
void ReadNumber(char* value, float lo, float hi, float& target) {
    for (char* c = value; *c; ++c) {
        if (*c == ',') *c = '.';
    }
    float number = 0.f;
    if (sscanf_s(value, "%f", &number) == 1 && number >= lo && number <= hi) target = number;
}

bool IniPath(wchar_t* path, const wchar_t* fileName, bool besideDll) {
    const HMODULE module = besideDll ? g_module : nullptr;
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    return wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), fileName) == 0;
}

void LoadConfig() {
    g_enabled = 1;
    g_diag = 0;
    g_maxSlopeRad = 0.f;
    g_checkRay = 1;
    g_checkContact = 1;
    g_checkWorld = 1;
    g_checkAvoid = 1;
    g_stepAllowance = kDefaultStepHeight;
    g_probeStep = kDefaultProbeStep;
    g_probeReach = kDefaultProbeReach;
    g_probeUp = kDefaultProbeUp;
    g_probeDown = kDefaultProbeDown;
    g_wallRayLength = kDefaultWallRayLength;
    g_wallRayHeight = kDefaultWallRayHeight;
    g_maxDiagLines = kDefaultDiagLines;

    // VobWay.ini beside the DLL, then VobWay.ini in System\, then the [VOBWAY] block of Gothic.ini.
    wchar_t path[MAX_PATH];
    const bool besideDll = IniPath(path, L"VobWay.ini", true) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    const bool inSystem = !besideDll && IniPath(path, L"VobWay.ini", false) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    if (!besideDll && !inSystem && !IniPath(path, L"Gothic.ini", false)) return;

    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const DWORD size = GetFileSize(file, nullptr);
    if (size == INVALID_FILE_SIZE || size > 2u * 1024u * 1024u) {
        CloseHandle(file);
        return;
    }
    char* buf = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, static_cast<size_t>(size) + 1));
    if (!buf) {
        CloseHandle(file);
        return;
    }
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buf, size, &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        HeapFree(GetProcessHeap(), 0, buf);
        return;
    }
    buf[read] = 0;
    const char* p = buf;
    // UTF-16 cannot be parsed, so Diag is unknown here and nothing is logged.
    if (read >= 2 && static_cast<unsigned char>(p[0]) == 0xFF && static_cast<unsigned char>(p[1]) == 0xFE) {
        HeapFree(GetProcessHeap(), 0, buf);
        return;
    }
    if (read >= 3 && static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB && static_cast<unsigned char>(p[2]) == 0xBF) {
        p += 3;
    }

    bool inSection = false;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\r' || *p == '\n') {
            SkipLine(p);
            continue;
        }
        if (*p == ';' || *p == '#') {
            SkipLine(p);
            continue;
        }
        if (*p == '[') {
            const char* name = p + 1;
            const char* end = name;
            while (*end && *end != ']' && *end != '\n' && *end != '\r') ++end;
            Trim(name, end);
            inSection = KeyIs(name, end, "VOBWAY");
            SkipLine(p);
            continue;
        }
        if (!inSection) {
            SkipLine(p);
            continue;
        }
        const char* key = p;
        while (*p && *p != '=' && *p != '\n' && *p != '\r' && *p != ';') ++p;
        const char* keyEnd = p;
        if (*p != '=') {
            SkipLine(p);
            continue;
        }
        ++p;
        const char* value = p;
        while (*p && *p != '\n' && *p != '\r' && *p != ';') ++p;
        const char* valueEnd = p;
        Trim(key, keyEnd);
        Trim(value, valueEnd);
        char tmp[64];
        const size_t n = static_cast<size_t>(valueEnd - value);
        if (n >= sizeof(tmp)) {
            SkipLine(p);
            continue;
        }
        memcpy(tmp, value, n);
        tmp[n] = 0;
        if (KeyIs(key, keyEnd, "Enabled")) {
            ReadSwitch(tmp, g_enabled);
        } else if (KeyIs(key, keyEnd, "Diag")) {
            ReadSwitch(tmp, g_diag);
        } else if (KeyIs(key, keyEnd, "CheckRay")) {
            ReadSwitch(tmp, g_checkRay);
        } else if (KeyIs(key, keyEnd, "CheckContact")) {
            ReadSwitch(tmp, g_checkContact);
        } else if (KeyIs(key, keyEnd, "CheckWorld")) {
            ReadSwitch(tmp, g_checkWorld);
        } else if (KeyIs(key, keyEnd, "CheckAvoid")) {
            ReadSwitch(tmp, g_checkAvoid);
        } else if (KeyIs(key, keyEnd, "MaxSlope")) {
            float degrees = 0.f;
            ReadNumber(tmp, 0.f, 89.f, degrees);
            g_maxSlopeRad = degrees > 0.f && degrees < 89.f ? degrees * kPi / 180.f : 0.f;
        } else if (KeyIs(key, keyEnd, "StepHeight")) {
            ReadNumber(tmp, 0.f, 100.f, g_stepAllowance);
        } else if (KeyIs(key, keyEnd, "ProbeStep")) {
            ReadNumber(tmp, 5.f, 100.f, g_probeStep);
        } else if (KeyIs(key, keyEnd, "ProbeReach")) {
            ReadNumber(tmp, 20.f, 400.f, g_probeReach);
        } else if (KeyIs(key, keyEnd, "ProbeUp")) {
            ReadNumber(tmp, 50.f, 500.f, g_probeUp);
        } else if (KeyIs(key, keyEnd, "ProbeDown")) {
            ReadNumber(tmp, 10.f, 500.f, g_probeDown);
        } else if (KeyIs(key, keyEnd, "WallRayLength")) {
            ReadNumber(tmp, 0.f, 300.f, g_wallRayLength);
        } else if (KeyIs(key, keyEnd, "WallRayHeight")) {
            ReadNumber(tmp, 0.f, 300.f, g_wallRayHeight);
        } else if (KeyIs(key, keyEnd, "DiagMaxLines")) {
            float lines = static_cast<float>(g_maxDiagLines);
            ReadNumber(tmp, 1.f, 1000000.f, lines);
            g_maxDiagLines = static_cast<int>(lines);
        }
        SkipLine(p);
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

void* ClassDefOf(void* vob) {
    if (!Readable(vob, 4)) return nullptr;
    void** vftable = *reinterpret_cast<void***>(vob);
    if (!Readable(vftable, 4)) return nullptr;
    using GetClassDef = void*(__thiscall*)(void* self);
    return reinterpret_cast<GetClassDef>(vftable[0])(vob);
}

int IsPlacedVob(void* vob) {
    void* def = ClassDefOf(vob);
    if (!def) return 0;
    for (int i = 0; i < 24; ++i) {
        if (!def) break;
        if (def == reinterpret_cast<void*>(kClassNpc)) return 0;
        if (def == reinterpret_cast<void*>(kClassMob)) return 0;
        if (def == reinterpret_cast<void*>(kClassItem)) return 0;
        if (def == reinterpret_cast<void*>(kClassMover)) return 0;
        if (def == reinterpret_cast<void*>(kClassLevelCompo)) return 0;
        if (!Readable(static_cast<uint8_t*>(def) + kClassBase, 4)) return 0;
        def = *reinterpret_cast<void**>(static_cast<uint8_t*>(def) + kClassBase);
    }
    return def ? 0 : 1;
}

float FieldOf(const void* base, uint32_t offset) {
    return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(base) + offset);
}

// MaxSlope only applies to vob surfaces; world mesh always gets the engine's own test.
int SlopeAllowed(void* ai, const float* normal, float angle, bool onVob) {
    if (onVob && g_maxSlopeRad > 0.f) return angle <= g_maxSlopeRad;
    return reinterpret_cast<CanWalkFn>(kCanWalkOnPoly)(ai, normal) != 0;
}

int SurfaceWalkable(void* ai, const uint8_t* report, float* outNormalY, bool onVob) {
    const auto* normal = reinterpret_cast<const float*>(report + kReportNormal);
    float ny = normal[1];
    *outNormalY = ny;
    if (!(ny > 0.f)) return 0;
    if (ny > 1.f) ny = 1.f;
    return SlopeAllowed(ai, normal, acosf(ny), onVob);
}

int WorldTraceNearest(void* world, const float* start, const float* ray, void* ignoreVob, int flags) {
    int hit;
    __asm {
        push flags
        push ignoreVob
        push ray
        mov edx, start
        mov ecx, world
        mov eax, kWorldTrace
        call eax
        mov hit, eax
    }
    return hit;
}

// Probes either one vob (its own TraceRay) or the whole world (world mesh and vobs).
struct Tracer {
    void* vob = nullptr;
    VobTraceRayFn vobTrace = nullptr;
    void* world = nullptr;
    void* npc = nullptr;
    int vobHits = 0;  // world mode: probes that landed on a placed vob
};

// 1 = hit, report filled in vob-report layout (+0x10 height, +0x18 normal); 0 = miss;
// -1 = the world ray hit a vob that must stay an obstacle (mob, item, mover, level
// composite). Characters are skipped by the trace itself (flag 0x800), as in the engine.
int Trace(Tracer& t, const float* start, const float* ray, uint8_t* report) {
    if (t.vob) return t.vobTrace(t.vob, start, ray, kTraceWantNormal, report) ? 1 : 0;
    if (!WorldTraceNearest(t.world, start, ray, t.npc, kWorldTraceFlags)) return 0;
    const auto* world = static_cast<const uint8_t*>(t.world);
    void* hitVob = *reinterpret_cast<void* const*>(world + kWorldReportVob);
    if (hitVob && !IsPlacedVob(hitVob)) return -1;
    memcpy(report + kReportHitY, world + kWorldReportHitY, 4);
    memcpy(report + kReportNormal, world + kWorldReportNormal, 12);
    return hitVob ? 2 : 1;
}

// At least one probe, even when ProbeStep is larger than ProbeReach.
int ProbeCount() {
    int count = static_cast<int>(g_probeReach / g_probeStep + 0.01f);
    if (count < 1) count = 1;
    return count > kMaxProbes ? kMaxProbes : count;
}

// Walks the height profile from the feet. Missing is fine, the ramp may end
// there, but at least one probe has to land.
// dir = probe direction (only x/z used); nullptr = the NPC's facing.
int ProfileWalkable(void* npc, Tracer& t, float* outNormalY, float* outRise, const float* dir) {
    void* ai = *reinterpret_cast<void**>(static_cast<uint8_t*>(npc) + kNpcAi);
    if (!Readable(ai, kAiSlopeLimit + 4)) return 0;
    void* aniCtrl = *reinterpret_cast<void**>(static_cast<uint8_t*>(npc) + kNpcAniCtrl);
    if (!Readable(aniCtrl, kAniCtrlFeetY + 4)) return 0;

    float limit = g_maxSlopeRad > 0.f ? g_maxSlopeRad : FieldOf(ai, kAiSlopeLimit);
    if (!(limit > 0.f)) return 0;
    if (limit > kMaxSlopeLimitRad) limit = kMaxSlopeLimitRad;
    const float climbPerUnit = tanf(limit);

    const float feetY = FieldOf(aniCtrl, kAniCtrlFeetY);
    const float posX = FieldOf(npc, kVobPosX);
    const float posZ = FieldOf(npc, kVobPosZ);
    float atX = dir ? dir[0] : FieldOf(npc, kVobAtX);
    float atZ = dir ? dir[2] : FieldOf(npc, kVobAtZ);
    const float atLength = sqrtf(atX * atX + atZ * atZ);
    if (!(atLength > 0.1f)) return 0;
    atX /= atLength;
    atZ /= atLength;

    // WallRayLength=0 switches the wall rays off.
    if (g_wallRayLength > 0.f) {
        const float wallRay[3] = {atX * g_wallRayLength, 0.f, atZ * g_wallRayLength};
        const float wallHeights[2] = {FieldOf(npc, kVobPosY), feetY + g_wallRayHeight};
        for (const float height : wallHeights) {
            const float wallStart[3] = {posX, height, posZ};
            uint8_t wallReport[64] = {};
            const int wall = Trace(t, wallStart, wallRay, wallReport);
            if (wall < 0 || (wall > 0 && !SurfaceWalkable(ai, wallReport, outNormalY, t.vob || wall == 2))) return 0;
        }
    }

    float previousY = feetY;
    float previousDistance = 0.f;
    float lastProbeDistance = 0.f;
    int hits = 0;
    bool gap = false;
    for (int i = 1; i <= ProbeCount(); ++i) {
        const float distance = i * g_probeStep;
        const float start[3] = {posX + atX * distance, feetY + g_probeUp, posZ + atZ * distance};
        const float ray[3] = {0.f, -(g_probeUp + g_probeDown), 0.f};
        uint8_t report[64] = {};
        const int probe = Trace(t, start, ray, report);
        if (probe < 0) return 0;
        if (probe == 0) {
            gap = true;
            lastProbeDistance = distance;
            continue;
        }
        if (probe == 2) ++t.vobHits;

        const float hitY = FieldOf(report, kReportHitY);
        *outRise = hitY - feetY;
        if (!SurfaceWalkable(ai, report, outNormalY, t.vob || probe == 2)) return 0;
        float allowed;
        if (gap) {
            // After a miss the surface begins somewhere after the last missed probe. A ramp
            // may have risen with its own slope since then, a flat crate top only gets the step.
            const float ny = *outNormalY;
            const float hitClimbPerUnit = ny < 1.f ? sqrtf(1.f - ny * ny) / ny : 0.f;
            allowed = (distance - lastProbeDistance) * hitClimbPerUnit + g_stepAllowance;
        } else {
            allowed = (distance - previousDistance) * climbPerUnit + g_stepAllowance;
        }
        if (hitY - previousY > allowed) return 0;
        gap = false;
        previousY = hitY;
        previousDistance = distance;
        lastProbeDistance = distance;
        ++hits;
    }
    return hits > 0;
}

int GroundWalkable(void* npc, void* vob, float* outNormalY, float* outRise, const float* dir = nullptr) {
    if (!IsPlacedVob(vob)) return 0;
    void** vftable = *reinterpret_cast<void***>(vob);
    if (!Readable(vftable + kVobTraceRaySlot, 4)) return 0;
    Tracer t;
    t.vob = vob;
    t.vobTrace = reinterpret_cast<VobTraceRayFn>(vftable[kVobTraceRaySlot]);
    return ProfileWalkable(npc, t, outNormalY, outRise, dir);
}

// The engine's ray hit a world polygon it cannot walk on. Pure world mesh keeps that
// result; only when a placed vob forms part of the ground ahead is the profile judged.
int WorldWalkable(void* npc, float* outNormalY, float* outRise, const float* dir = nullptr) {
    void* world = *reinterpret_cast<void**>(static_cast<uint8_t*>(npc) + kNpcWorld);
    if (!Readable(world, kWorldReportNormal + 12)) return 0;
    Tracer t;
    t.world = world;
    t.npc = npc;
    return ProfileWalkable(npc, t, outNormalY, outRise, dir) && t.vobHits > 0;
}

// Log lines are built with truncating calls only: the *_s functions without _TRUNCATE
// terminate the game on overflow, which __try does not catch.
void Append(char* line, size_t size, const char* text) {
    strncat_s(line, size, text, _TRUNCATE);
}

// " label (x y z)" with the world position of a vob or NPC, as the Spacer shows it.
void AppendPos(char* line, size_t size, const char* label, void* vob) {
    char part[160];
    if (vob && Readable(static_cast<uint8_t*>(vob) + kVobPosX, kVobPosZ - kVobPosX + 4)) {
        _snprintf_s(part, _TRUNCATE, " %s (%.0f %.0f %.0f)", label, FieldOf(vob, kVobPosX), FieldOf(vob, kVobPosY), FieldOf(vob, kVobPosZ));
    } else {
        _snprintf_s(part, _TRUNCATE, " %s -", label);
    }
    Append(line, size, part);
}

// Collision flags at +0x12D that make CanGo take its own branch (0x680BC2..0x680C08).
void AppendFlags(char* line, size_t size, unsigned flags) {
    char part[32];
    _snprintf_s(part, _TRUNCATE, " flags=%02X", flags);
    Append(line, size, part);
    if (flags & 0x01) Append(line, size, " ceiling");
    if (flags & 0x06) Append(line, size, " slope");
    if (flags & 0x08) Append(line, size, " step-too-high");
    if (flags & 0x10) Append(line, size, " drop");
}

// Consecutive decisions with the same NPC, vob and result are logged once. The site is
// left out on purpose: an NPC pressing against a vob alternates Ray and Contact every frame.
void DiagVob(const char* site, void* npc, void* vob, float normalY, float rise, int walk) {
    if (!g_diag || g_diagLines >= g_maxDiagLines) return;
    if (npc == g_lastDiagNpc && vob == g_lastVob && walk == g_lastWalk) return;
    g_lastDiagNpc = npc;
    g_lastVob = vob;
    g_lastWalk = walk;
    ++g_diagLines;
    char line[1024];
    _snprintf_s(line, _TRUNCATE, "VobWay: %s %s", site, walk ? "walkable=1 (way free)" : "walkable=0 (engine decides)");
    AppendPos(line, sizeof(line), "NPC", npc);
    AppendPos(line, sizeof(line), "VOB", vob);
    char part[160];
    _snprintf_s(part, _TRUNCATE, " vob=%p normalY=%.3f rise=%.1f", vob, normalY, rise);
    Append(line, sizeof(line), part);
    Log(line);
}

// Any failure keeps the engine result, an obstacle.
int JudgeVob(const char* site, void* npc, void* vob) {
    int walk = 0;
    float normalY = 0.f;
    float rise = 0.f;
    __try {
        // An NPC, mob, item, mover or level composite keeps the engine result; the world
        // probe below skips characters and may pass beside the obstacle, so it must not
        // overrule it.
        if (!IsPlacedVob(vob)) {
            DiagVob(site, npc, vob, normalY, rise, 0);
            return 0;
        }
        // The hit vob alone, then the whole ground ahead: a ramp vob may lead onto the
        // vob the ray hit (a second platform), which the hit vob alone cannot show.
        walk = GroundWalkable(npc, vob, &normalY, &rise);
        if (!walk && g_checkWorld) walk = WorldWalkable(npc, &normalY, &rise);
        DiagVob(site, npc, vob, normalY, rise, walk);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        walk = 0;
        if (g_diagLines < g_maxDiagLines) {
            ++g_diagLines;
            Log("VobWay: exception while probing a vob, engine result kept (obstacle).");
        }
    }
    return walk;
}

// CheckRay, CheckContact and CheckWorld=0 keep the engine result at that site.
int __cdecl DecideWorldHit(void* npc) {
    if (!g_checkWorld) return 0;
    int walk = 0;
    float normalY = 0.f;
    float rise = 0.f;
    __try {
        walk = WorldWalkable(npc, &normalY, &rise);
        DiagVob("World", npc, nullptr, normalY, rise, walk);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        walk = 0;
        if (g_diagLines < g_maxDiagLines) {
            ++g_diagLines;
            Log("VobWay: exception while probing the world, engine result kept (obstacle).");
        }
    }
    return walk;
}

int __cdecl DecideVobHit(void* npc, void* vob) {
    if (!g_checkRay) return 0;
    return JudgeVob("Ray", npc, vob);
}

int __cdecl DecideContact(void* npc) {
    if (!g_checkContact) return 0;
    void* vob = nullptr;
    __try {
        void* ai = *reinterpret_cast<void**>(static_cast<uint8_t*>(npc) + kNpcAi);
        void* selfVob = *reinterpret_cast<void**>(static_cast<uint8_t*>(ai) + kAiVob);
        void* coll = *reinterpret_cast<void**>(static_cast<uint8_t*>(selfVob) + kVobCollision);
        vob = *reinterpret_cast<void**>(static_cast<uint8_t*>(coll) + kCollFoundVob);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return vob ? JudgeVob("Contact", npc, vob) : 0;
}

// Entered from the redirected jne at 0x680E6F. Registers there: edx = &hit vob,
// esi = NPC, ebx != 0 when the first trace hit an interactive object. Both targets
// reload eax, ecx and edx themselves. The x87 stack is empty here (both paths into
// 0x680E6F come after an fstp), and the engine itself read [edx] at 0x680E6C. A
// walkable hit continues as if the rays had missed, so the engine and ContactStub
// still judge what the body is touching.
#pragma runtime_checks("", off)
__declspec(naked) void VobHitStub() {
    __asm {
        test ebx, ebx
        jne obstacle
        mov eax, [edx]
        push eax
        push esi
        call DecideVobHit
        add esp, 8
        test eax, eax
        jne walk
    obstacle:
        jmp dword ptr [g_toObstacle]
    walk:
        jmp dword ptr [g_toRayClear]
    }
}

// Entered by jmp from 0x680DF2. esi = NPC, x87 stack empty. On refusal it runs the
// two overwritten instructions and continues in the engine.
__declspec(naked) void ContactStub() {
    __asm {
        push esi
        call DecideContact
        add esp, 4
        test eax, eax
        jne walk
        fld dword ptr [esi + 0x64]
        sub esp, 0x0C
        jmp dword ptr [g_toContactResume]
    walk:
        jmp dword ptr [g_toNoContact]
    }
}

// Entered from the redirected je at 0x680EE5, only when CanWalkOnPoly refused the world
// polygon the ray hit. esi = NPC, x87 stack empty, same stack depth as 0x680DAD. A walkable
// vob ground continues as if the rays had missed, like VobHitStub.
__declspec(naked) void WorldHitStub() {
    __asm {
        push esi
        call DecideWorldHit
        add esp, 4
        test eax, eax
        jne walk
        jmp dword ptr [g_toObstacle]
    walk:
        jmp dword ptr [g_toRayClear]
    }
}
#pragma runtime_checks("", restore)

void DiagCanGo(void* npc, int result) {
    if (!Readable(static_cast<uint8_t*>(npc) + kNpcTraceVob, 4)) return;
    if (npc == g_lastNpc && result == g_lastResult) return;
    g_lastNpc = npc;
    g_lastResult = result;
    // Only refusals: CanGo 0 (free way) is the normal case and would flood the log.
    if (result == 0 || g_diagLines >= g_maxDiagLines) return;
    ++g_diagLines;

    void* ai = *reinterpret_cast<void**>(static_cast<uint8_t*>(npc) + kNpcAi);
    void* traceVob = *reinterpret_cast<void**>(static_cast<uint8_t*>(npc) + kNpcTraceVob);
    void* foundPoly = nullptr;
    void* foundVob = nullptr;
    unsigned flags = 0;
    if (Readable(static_cast<uint8_t*>(ai) + kAiVob, 4)) {
        void* selfVob = *reinterpret_cast<void**>(static_cast<uint8_t*>(ai) + kAiVob);
        if (Readable(static_cast<uint8_t*>(selfVob) + kVobCollision, 4)) {
            void* coll = *reinterpret_cast<void**>(static_cast<uint8_t*>(selfVob) + kVobCollision);
            if (Readable(static_cast<uint8_t*>(coll) + kCollFlags, 1)) {
                foundPoly = *reinterpret_cast<void**>(static_cast<uint8_t*>(coll) + kCollFoundPoly);
                foundVob = *reinterpret_cast<void**>(static_cast<uint8_t*>(coll) + kCollFoundVob);
                flags = *(static_cast<uint8_t*>(coll) + kCollFlags);
            }
        }
    }
    // Meaning as RbtAvoidObstacles (0x6875E0) acts on it.
    const char* meaning = result == 2 ? "avoid (turns on the spot)"
        : result == 3 ? "jump/climb"
        : result == 4 ? "turn back (drop)"
        : "special branch";
    char line[1024];
    _snprintf_s(line, _TRUNCATE, "VobWay: CanGo=%d %s", result, meaning);
    AppendPos(line, sizeof(line), "NPC", npc);
    AppendPos(line, sizeof(line), "ray-VOB", traceVob);
    AppendPos(line, sizeof(line), "contact-VOB", foundVob);
    Append(line, sizeof(line), foundPoly ? " contact-world-polygon=yes" : " contact-world-polygon=no");
    AppendFlags(line, sizeof(line), flags);
    char ids[64];
    _snprintf_s(ids, _TRUNCATE, " npc=%p rayVob=%p", npc, traceVob);
    Append(line, sizeof(line), ids);
    Log(line);
}

int __fastcall CanGoDiag(void* npc, void*, uint32_t distance, float* normal) {
    const int result = reinterpret_cast<CanGoFn>(kCanGo)(npc, distance, normal);
    __try {
        DiagCanGo(npc, result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

// Replaces the RbtGetObstacle call in RbtCheckLastDirection (ret 10h like the original).
// A hit on a walkable vob ground along the stored direction counts as free, so the NPC
// turns back onto its original heading instead of staying on the detour.
int __fastcall LastDirObstacle(void* npc, void*, const float* start, const float* dir, void** hitVob, float* normal) {
    const int hit = reinterpret_cast<GetObstacleFn>(kRbtGetObstacle)(npc, start, dir, hitVob, normal);
    if (!hit || !g_checkAvoid) return hit;
    int walk = 0;
    float normalY = 0.f;
    float rise = 0.f;
    void* vob = nullptr;
    __try {
        vob = *hitVob;
        // As in JudgeVob: an NPC, mob, item, mover or level composite stays an obstacle.
        // Without a vob the hit is world mesh, which only the world probe may clear.
        if (!vob || IsPlacedVob(vob)) {
            if (vob) walk = GroundWalkable(npc, vob, &normalY, &rise, dir);
            if (!walk && g_checkWorld) walk = WorldWalkable(npc, &normalY, &rise, dir);
        }
        DiagVob("Avoid", npc, vob, normalY, rise, walk);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        walk = 0;
        if (g_diagLines < g_maxDiagLines) {
            ++g_diagLines;
            Log("VobWay: exception while probing in avoid mode, engine result kept (obstacle).");
        }
    }
    if (!walk) return hit;
    *hitVob = nullptr;
    return 0;
}

bool Patch(void* address, const void* bytes, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(address, bytes, size);
    DWORD ignored = 0;
    VirtualProtect(address, size, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    return true;
}

void InstallDiag() {
    auto* callSite = reinterpret_cast<uint8_t*>(kCanGoCall);
    const uint8_t kCall[5] = {0xE8, 0xF8, 0x94, 0xFF, 0xFF};
    if (memcmp(callSite, kCall, 5) != 0) {
        Log("VobWay: Diag off, call site does not match.");
        return;
    }
    memcpy(g_callSaved, callSite, 5);
    const intptr_t rel = reinterpret_cast<uint8_t*>(&CanGoDiag) - (callSite + 5);
    uint8_t call[5] = {0xE8, 0, 0, 0, 0};
    memcpy(call + 1, &rel, 4);
    if (Patch(callSite, call, 5)) {
        g_diagPatched = 1;
        char line[96];
        sprintf_s(line, "VobWay: Diag on, log limited to %d lines.", g_maxDiagLines);
        Log(line);
    }
}

void Install() {
    LoadConfig();
    if (!SameCode()) return;
    if (!g_enabled) {
        Log("VobWay: Enabled=0, no patch.");
        return;
    }

    auto* jne = reinterpret_cast<uint8_t*>(kVobHitJne);
    auto* contact = reinterpret_cast<uint8_t*>(kContactBlock);
    auto* worldJe = reinterpret_cast<uint8_t*>(kWorldPolyJe);
    auto* lastDir = reinterpret_cast<uint8_t*>(kLastDirCall);
    const uint8_t kJne[6] = {0x0F, 0x85, 0x3C, 0x03, 0x00, 0x00};
    const uint8_t kContact[6] = {0xD9, 0x46, 0x64, 0x83, 0xEC, 0x0C};
    const uint8_t kWorldJe[6] = {0x0F, 0x84, 0xC6, 0x02, 0x00, 0x00};
    const uint8_t kLastDir[5] = {0xE8, 0x84, 0xF1, 0xFF, 0xFF};
    if (memcmp(jne, kJne, 6) != 0 || memcmp(contact, kContact, 6) != 0 || memcmp(worldJe, kWorldJe, 6) != 0 ||
        memcmp(lastDir, kLastDir, 5) != 0) {
        Log("VobWay: opcode check failed. No patch.");
        return;
    }
    memcpy(g_jneSaved, jne, 6);
    memcpy(g_contactSaved, contact, 6);
    memcpy(g_worldSaved, worldJe, 6);
    memcpy(g_lastDirSaved, lastDir, 5);

    const intptr_t jneRel = reinterpret_cast<uint8_t*>(&VobHitStub) - (jne + 6);
    uint8_t jnePatched[6] = {0x0F, 0x85, 0, 0, 0, 0};
    memcpy(jnePatched + 2, &jneRel, 4);
    const intptr_t contactRel = reinterpret_cast<uint8_t*>(&ContactStub) - (contact + 5);
    uint8_t contactPatched[6] = {0xE9, 0, 0, 0, 0, 0x90};
    memcpy(contactPatched + 1, &contactRel, 4);
    const intptr_t worldRel = reinterpret_cast<uint8_t*>(&WorldHitStub) - (worldJe + 6);
    uint8_t worldPatched[6] = {0x0F, 0x84, 0, 0, 0, 0};
    memcpy(worldPatched + 2, &worldRel, 4);
    const intptr_t lastDirRel = reinterpret_cast<uint8_t*>(&LastDirObstacle) - (lastDir + 5);
    uint8_t lastDirPatched[5] = {0xE8, 0, 0, 0, 0};
    memcpy(lastDirPatched + 1, &lastDirRel, 4);
    if (!Patch(jne, jnePatched, 6) || !Patch(contact, contactPatched, 6) || !Patch(worldJe, worldPatched, 6) ||
        !Patch(lastDir, lastDirPatched, 5)) {
        Patch(jne, g_jneSaved, 6);
        Patch(contact, g_contactSaved, 6);
        Patch(worldJe, g_worldSaved, 6);
        Patch(lastDir, g_lastDirSaved, 5);
        Log("VobWay: writing failed. No patch.");
        return;
    }
    g_patched = 1;

    char slope[24];
    if (g_maxSlopeRad > 0.f) {
        sprintf_s(slope, "%.1f deg", g_maxSlopeRad * 180.f / kPi);
    } else {
        sprintf_s(slope, "Engine");
    }
    char line[320];
    sprintf_s(line, "VobWay " VOBWAY_VERSION ": active. MaxSlope=%s (vobs only). CheckRay=%d CheckContact=%d CheckWorld=%d CheckAvoid=%d StepHeight=%.0f "
        "ProbeStep=%.0f ProbeReach=%.0f (%d points) ProbeUp=%.0f ProbeDown=%.0f WallRayLength=%.0f WallRayHeight=%.0f",
        slope, g_checkRay, g_checkContact, g_checkWorld, g_checkAvoid, g_stepAllowance, g_probeStep, g_probeReach, ProbeCount(),
        g_probeUp, g_probeDown, g_wallRayLength, g_wallRayHeight);
    Log(line);
    if (g_diag) InstallDiag();
}

void Remove() {
    if (g_diagPatched) {
        Patch(reinterpret_cast<void*>(kCanGoCall), g_callSaved, 5);
        g_diagPatched = 0;
    }
    if (g_patched) {
        Patch(reinterpret_cast<void*>(kVobHitJne), g_jneSaved, 6);
        Patch(reinterpret_cast<void*>(kContactBlock), g_contactSaved, 6);
        Patch(reinterpret_cast<void*>(kWorldPolyJe), g_worldSaved, 6);
        Patch(reinterpret_cast<void*>(kLastDirCall), g_lastDirSaved, 5);
        g_patched = 0;
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        Install();
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        Remove();
    }
    return TRUE;
}
