// Underglow: underglow neon for Forza Horizon 2, made of the game's own lights.
//
// How FH2 lights a car
// --------------------
// The renderer keeps the lights of a scene in a LightManager (vtable 0x2E7E600). A car gets its lights from it
// when its presentation is set up, and moves them every frame through the manager's virtual methods:
//
//   slot 3 0x10C7E00 CreateLight(mgr, uint* handle, key, index): `key` picks a light set registered with the
//          manager (mgr+0x60), `index` an entry in it (a beam preset and a glow preset). Allocates a mapping slot
//          (the handle, mgr+0x28, 24 bytes each: +0 beam record, +4 glow record) and the two records.
//   slot 5 0x10F0550 SetIntensity(mgr, handle, float)     beam record +0x10, and the glow's (0x10F04C0)
//   slot 6 0x10F1C30 SetPosition(mgr, handle, float4*)    beam record +0x14..+0x1C (world, y up)
//   slot 7 0x10F0DA0 SetDirection(mgr, handle, float4*)   beam record +0x20 (a world rotation quaternion, xyzw)
//
// The car's presentation (CarPresentation11, ForzaCar+0x4D38 -> +8) calls those for each of its lights once a
// frame, one light after the other (position, direction, intensity), from game+0x13B6E85.
//
//   manager+0x08 beam system: records pool at +0x00 (0x40 each: +0x10 intensity, +0x14 position, +0x20 rotation,
//                +0x30 preset), presets pool at +0x38 (0xC0 each: +0x30 users, +0x34 GPU slot, +0x50 rgb,
//                +0x60 range, +0x64 power, +0x68 inner cone, +0x6C outer cone, +0x9C/+0xA0 day threshold),
//                +0x100 "presets changed" (the preset upload 0x10F6C40 only runs then).
//                0x10BE480(pool) allocates a preset, 0x10F1EB0(system, record, preset) gives a record a preset.
//   manager+0x18 glow system (the lens sprites): 0x10F04C0(system, glow record, float) sets one's intensity.
//   The car's own light instances: presentation+0x1278 asset -> +0x4BE0 vector of instance pointers, each with
//   +0x04 type (13 headlight, 69 tail glow), +0x10 position and +0x20 rotation on the car, +0x50 handle.
//
// The neon
// --------
// When the game sets the intensity of the player's reference headlight (the last of its three calls that frame)
// we know where that light is in the world and where it is on the car, so we know the car's transform. Right
// there, on the game's own light thread, we create our lights the first time (CreateLight with the tail light's
// key and index, then a preset of our own: the tail light's, recolored, always on) and place them in a row
// under the car pointing down, with their glow sprites off.
#include "fh2mod.h"
#include "fh2game.h"
#include "fh2anna.h"

#include <windows.h>

#include <intrin.h>
#include <xmmintrin.h>
#include <share.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "neon_text.h"

#ifndef UNDERGLOW_VERSION
#define UNDERGLOW_VERSION "0.0.0"
#endif

namespace {

const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod = nullptr;
uintptr_t g_base = 0;
wchar_t g_ini[MAX_PATH] = {};
wchar_t g_mods_dir[MAX_PATH] = {};

// ---- Game addresses -------------------------------------------------------------------------------------------

const uintptr_t kBeamUpload = 0x10F6C40;   // void(system, graphics): presets to the GPU
const uintptr_t kCreateLight = 0x10C7E00, kSetIntensity = 0x10F0550, kSetPosition = 0x10F1C30,
                kSetDirection = 0x10F0DA0;
const uintptr_t kGlowSetIntensity = 0x10F04C0;   // (glow system, glow record, float)
const uintptr_t kAttachPreset = 0x10F1EB0;       // (beam system, record, preset)
const uintptr_t kAllocBeamPreset = 0x10BE480;    // uint(pool = beam system + 0x38)
const uintptr_t kMatrixToQuat = 0x10F89B0;       // __m128(const float4x4*): the rotation of a world matrix
const uintptr_t kPresWorld = 0x1A0;              // CarPresentation11: the car's world matrix (rows x, y, z, position)

const uintptr_t kForzaCarKindOffset = 0x4D08;
const uintptr_t kForzaCarPresOffset = 0x4D38;
const uintptr_t kDeferredPresVtable = 0x2EBEF68;
const uintptr_t kDeferredPresOffset = 0x8;
const uintptr_t kCarPresVtables[] = {0x2EC1450, 0x2EC2B70};

const uintptr_t kPresContext = 0x10, kContextManager = 0x380, kPresAsset = 0x1278;
const uintptr_t kAssetInstances = 0x4BE0;
const uintptr_t kManagerBeams = 0x08, kManagerGlows = 0x18, kManagerMapping = 0x28;
const size_t kMappingStride = 24;
const uint16_t kTypeHeadlight = 13, kTypeTailGlow = 69;

const uintptr_t kBeamPresetPool = 0x38, kBeamDirty = 0x100;
const size_t kRecordStride = 0x40;
const uintptr_t kRecIntensity = 0x10, kRecPos = 0x14, kRecRot = 0x20, kRecPreset = 0x30;
const size_t kBeamPresetStride = 0xC0;
const uintptr_t kPreUsers = 0x30, kPreGpu = 0x34, kPreRgb = 0x50, kPreRange = 0x60, kPrePower = 0x64,
                kPreInner = 0x68, kPreOuter = 0x6C, kPreThreshold = 0x9C;
const size_t kMaxRecords = 100000;

struct Kind {
    const char* name;
    uintptr_t manager_slot;
    uintptr_t records;
    uintptr_t record_preset;
    uint32_t preset_mask;
    uintptr_t presets;
    size_t preset_stride;
    uintptr_t mapping_field;
};
const Kind kBeam = {"beam", kManagerBeams, 0x000, 0x30, 0xFFFFFFFFu, 0x038, 0xC0, 0};

using UploadFn = void (*)(void* system, void* graphics);
using CreateLightFn = uint32_t* (*)(void* mgr, uint32_t* handle, uint32_t key, uint32_t index);
using SetVecFn = void (*)(void* mgr, uint32_t handle, const float* v);
using SetFloatFn = void (*)(void* mgr, uint32_t handle, float v);
using GlowIntensityFn = void (*)(void* glow_system, uint32_t record, float v);
using AttachPresetFn = void (*)(void* beam_system, uint32_t record, uint32_t preset);
using AllocPresetFn = uint32_t (*)(void* pool);
using MatrixToQuatFn = __m128 (*)(const float* matrix);
using CarUpdateFn = void (*)(void* car, float dt, float b, uint32_t flag);

UploadFn g_orig_beam = nullptr;
CreateLightFn g_orig_create = nullptr;
SetVecFn g_orig_set_pos = nullptr, g_orig_set_dir = nullptr;
SetFloatFn g_orig_set_int = nullptr;
CarUpdateFn g_orig_car_update = nullptr;

// ---- Memory that may not be there -----------------------------------------------------------------------------

bool rd(uintptr_t p, void* out, size_t n) {
    if (!p) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(p), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool wr(uintptr_t p, const void* in, size_t n) {
    if (!p) return false;
    __try {
        memcpy(reinterpret_cast<void*>(p), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool rdp(uintptr_t p, uintptr_t* out) { return rd(p, out, 8) && *out; }

bool vtable_is(uintptr_t obj, const uintptr_t* rvas, int n) {
    uintptr_t vt = 0;
    if (!rd(obj, &vt, 8)) return false;
    for (int i = 0; i < n; ++i)
        if (vt == g_base + rvas[i]) return true;
    return false;
}

struct Vec {
    uintptr_t begin, end, cap;
};

bool read_vec(uintptr_t at, size_t stride, size_t limit, Vec* v, size_t* count) {
    if (!rd(at, v, sizeof *v) || !v->begin || v->end < v->begin || (v->end - v->begin) % stride) return false;
    *count = (v->end - v->begin) / stride;
    return *count <= limit;
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// The mapping entry of a handle: its beam and glow record indices.
bool mapping_of(uintptr_t manager, uint32_t handle, uint32_t* beam, uint32_t* glow) {
    Vec map;
    size_t n = 0;
    uint32_t e[2];
    if (!read_vec(manager + kManagerMapping, kMappingStride, kMaxRecords, &map, &n) || handle >= n ||
        !rd(map.begin + handle * kMappingStride, e, 8))
        return false;
    *beam = e[0];
    *glow = e[1];
    return true;
}

uintptr_t beam_record(uintptr_t system, uint32_t index) {
    Vec v;
    size_t n = 0;
    return read_vec(system, kRecordStride, kMaxRecords, &v, &n) && index < n ? v.begin + index * kRecordStride : 0;
}

uintptr_t beam_preset(uintptr_t system, uint32_t index) {
    Vec v;
    size_t n = 0;
    return read_vec(system + kBeamPresetPool, kBeamPresetStride, kMaxRecords, &v, &n) && index < n
               ? v.begin + index * kBeamPresetStride
               : 0;
}

// ---- Quaternions (x, y, z, w) ---------------------------------------------------------------------------------

void qmul(const float* a, const float* b, float* o) {
    float r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                  a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                  a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    memcpy(o, r, sizeof r);
}

void qrot(const float* q, const float* v, float* o) {
    float t[3] = {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
                  2.0f * (q[0] * v[1] - q[1] * v[0])};
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

bool finite(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

// ---- Settings -------------------------------------------------------------------------------------------------

float clamp01(float x);

// A color as the ini or a command gives it: "0 1 1" (0 to 1), "255 105 180" (0 to 255: any number above 1),
// or hex "#FF69B4" / "FF69B4". Commas work as well as spaces.
bool parse_color(const char* text, float out[3]) {
    char t[64];
    strncpy_s(t, text, _TRUNCATE);
    for (char& ch : t)
        if (ch == ',' || ch == '\t') ch = ' ';
    const char* p = t;
    while (*p == ' ') ++p;
    const char* hex = *p == '#' ? p + 1 : p;
    size_t n = 0;
    while (isxdigit((unsigned char)hex[n])) ++n;
    const char* after = hex + n;
    while (*after == ' ') ++after;
    if (n == 6 && !*after && (*p == '#' || strpbrk(hex, "abcdefABCDEF") || !strchr(p, ' '))) {
        char six[7] = {};
        memcpy(six, hex, 6);
        const unsigned long v = strtoul(six, nullptr, 16);
        out[0] = float((v >> 16) & 0xFF) / 255.0f;
        out[1] = float((v >> 8) & 0xFF) / 255.0f;
        out[2] = float(v & 0xFF) / 255.0f;
        return true;
    }
    float v[3];
    if (sscanf_s(p, "%f %f %f", &v[0], &v[1], &v[2]) != 3) return false;
    const bool bytes = v[0] > 1.0f || v[1] > 1.0f || v[2] > 1.0f;
    for (int i = 0; i < 3; ++i) out[i] = clamp01(bytes ? v[i] / 255.0f : v[i]);
    return true;
}

struct Color {
    const char* name;
    float rgb[3];
};
// The menus' colors: these, then the ini's ten custom ones ([CustomColors]). Settings keep a color by its
// place in that list (0.1 to 0.4 had nine colors and one Custom, at 9: see load_config).
const Color kColors[] = {
    {"Cyan", {0.0f, 1.0f, 1.0f}},    {"Blue", {0.0f, 0.25f, 1.0f}},   {"Purple", {0.6f, 0.0f, 1.0f}},
    {"Pink", {1.0f, 0.1f, 0.6f}},    {"Red", {1.0f, 0.0f, 0.0f}},     {"Orange", {1.0f, 0.35f, 0.0f}},
    {"Yellow", {1.0f, 0.85f, 0.0f}}, {"Green", {0.0f, 1.0f, 0.15f}},  {"White", {1.0f, 1.0f, 1.0f}},
    {"Lime", {0.55f, 1.0f, 0.0f}},   {"Teal", {0.0f, 0.75f, 0.6f}},   {"Sky Blue", {0.3f, 0.7f, 1.0f}},
    {"Indigo", {0.3f, 0.0f, 1.0f}},  {"Magenta", {1.0f, 0.0f, 1.0f}}, {"Crimson", {1.0f, 0.0f, 0.25f}},
    {"Amber", {1.0f, 0.6f, 0.0f}},   {"Mint", {0.4f, 1.0f, 0.6f}},    {"Lavender", {0.7f, 0.5f, 1.0f}},
};
const int kColorCount = int(sizeof kColors / sizeof kColors[0]);   // the preset colors
const int kCustomCount = 10;
const int kColorTotal = kColorCount + kCustomCount;                 // with the custom ones after them
const float kCustomDefaults[kCustomCount][3] = {
    {0.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 0.5f}, {1.0f, 0.8f, 0.5f}, {1.0f, 1.0f, 0.5f}, {0.5f, 1.0f, 0.5f},
    {0.5f, 1.0f, 1.0f}, {0.5f, 0.6f, 1.0f}, {0.8f, 0.5f, 1.0f}, {1.0f, 0.5f, 1.0f}, {1.0f, 0.9f, 0.8f},
};

enum Effect {
    kEffectStatic, kEffectRainbow, kEffectWave, kEffectBreathe, kEffectWipe, kEffectStack, kEffectBlink,
    kEffectPolice, kEffectAudio, kEffectCrossing, kEffectHypnotoad, kEffectLightning, kEffectMarquee,
    kEffectComet, kEffectFill, kEffectSwap, kEffectVisor,
    // 0.4: more that follow the radio. New effects go on the end: the ini keeps effects by number.
    kEffectAudioPulse, kEffectAudioPeak, kEffectAudioBlend, kEffectBassFlash, kEffectAudioSparkle,
    kEffectAudioRainbow, kEffectAudioCenter,
    // 0.5: after OpenRGB Effects Plugin's effects (written afresh, for a ring of lights round a car)
    kEffectAudioBubbles, kEffectAudioParty, kEffectAudioSine, kEffectAudioStar, kEffectAudioSync, kEffectVuMeter,
    kEffectAudioSwirl,
    kEffectBouncingBall, kEffectBreathingCircle, kEffectFractalMotion, kEffectMotionPoint, kEffectRain,
    kEffectRotatingBeam, kEffectSmoothBlink, kEffectSparkleFade, kEffectZigZag, kEffectMarquee1,
    kEffectAlternateBlink, kEffectGradientWave, kEffectMovingPanes, kEffectNoiseMap, kEffectSequence, kEffectWavy,
    kEffectTwoColorMarquee,
    kEffectBloom, kEffectBubbles, kEffectColorWheel, kEffectDoubleRainbow, kEffectMotionPoints, kEffectMosaic,
    kEffectRadialRainbow, kEffectRandomSpin, kEffectRotatingRainbow, kEffectSpiral, kEffectStarryNight,
    kEffectSunrise, kEffectSwirlCircles,
    kEffectCount
};
const char* const kEffects[kEffectCount] = {
    "Static", "Rainbow", "Rainbow Wave", "Breathe", "Wipe", "Stack", "Blink", "Police", "Audio Visualizer",
    "Crossing Beams", "Hypnotoad", "Lightning", "Random Marquee", "Comet", "Fill", "Swap", "Visor",
    "Audio Pulse", "Peak Meter", "Audio Blend", "Bass Flash", "Audio Sparkle", "Audio Rainbow", "Center Burst",
    "Audio Bubbles", "Audio Party", "Audio Sine", "Audio Star", "Audio Sync", "VU Meter", "Audio Swirl",
    "Bouncing Ball", "Breathing Circle", "Fractal Motion", "Motion Point", "Rain", "Rotating Beam", "Smooth Blink",
    "Sparkle Fade", "Zig Zag", "Marquee",
    "Alternate Blink", "Gradient Wave", "Moving Panes", "Noise Map", "Sequence", "Wavy", "Two-Color Marquee",
    "Bloom", "Bubbles", "Color Wheel", "Double Rainbow", "Motion Points", "Mosaic", "Radial Rainbow", "Random Spin",
    "Rotating Rainbow", "Spiral", "Starry Night", "Sunrise", "Swirl Circles",
};

// The effects in groups, for the menus (ANNA's submenus, the Mods page's list in this order).
enum EffectGroup { kGroupAudio, kGroupOneColor, kGroupTwoColors, kGroupNoColor, kGroupCount };
const char* const kGroupNames[kGroupCount] = {"Audio Visualizer", "Effects with 1 Color", "Effects with 2 Colors",
                                              "Multicolor Effects"};
const char* const kGroupKeys[kGroupCount] = {"audio", "one", "two", "none"};
struct GroupedEffect {
    int group, effect;
};
const GroupedEffect kEffectOrder[] = {
    {kGroupAudio, kEffectAudio}, {kGroupAudio, kEffectAudioPulse}, {kGroupAudio, kEffectAudioPeak},
    {kGroupAudio, kEffectAudioBlend}, {kGroupAudio, kEffectBassFlash}, {kGroupAudio, kEffectAudioSparkle},
    {kGroupAudio, kEffectAudioCenter}, {kGroupAudio, kEffectAudioRainbow}, {kGroupAudio, kEffectVuMeter},
    {kGroupAudio, kEffectAudioSine}, {kGroupAudio, kEffectAudioSync}, {kGroupAudio, kEffectAudioParty},
    {kGroupAudio, kEffectAudioStar}, {kGroupAudio, kEffectAudioBubbles}, {kGroupAudio, kEffectAudioSwirl},
    {kGroupOneColor, kEffectStatic}, {kGroupOneColor, kEffectBreathe}, {kGroupOneColor, kEffectSmoothBlink},
    {kGroupOneColor, kEffectBlink}, {kGroupOneColor, kEffectWipe}, {kGroupOneColor, kEffectFill},
    {kGroupOneColor, kEffectStack}, {kGroupOneColor, kEffectComet}, {kGroupOneColor, kEffectVisor},
    {kGroupOneColor, kEffectMarquee1}, {kGroupOneColor, kEffectMotionPoint}, {kGroupOneColor, kEffectBouncingBall},
    {kGroupOneColor, kEffectRain}, {kGroupOneColor, kEffectSparkleFade}, {kGroupOneColor, kEffectRotatingBeam},
    {kGroupOneColor, kEffectBreathingCircle}, {kGroupOneColor, kEffectZigZag},
    {kGroupOneColor, kEffectFractalMotion}, {kGroupOneColor, kEffectLightning},
    {kGroupTwoColors, kEffectCrossing}, {kGroupTwoColors, kEffectSwap}, {kGroupTwoColors, kEffectAlternateBlink},
    {kGroupTwoColors, kEffectGradientWave}, {kGroupTwoColors, kEffectSequence}, {kGroupTwoColors, kEffectWavy},
    {kGroupTwoColors, kEffectTwoColorMarquee}, {kGroupTwoColors, kEffectMovingPanes},
    {kGroupTwoColors, kEffectNoiseMap},
    {kGroupNoColor, kEffectRainbow}, {kGroupNoColor, kEffectWave}, {kGroupNoColor, kEffectColorWheel},
    {kGroupNoColor, kEffectRotatingRainbow}, {kGroupNoColor, kEffectDoubleRainbow},
    {kGroupNoColor, kEffectRadialRainbow}, {kGroupNoColor, kEffectSpiral}, {kGroupNoColor, kEffectRandomSpin},
    {kGroupNoColor, kEffectHypnotoad}, {kGroupNoColor, kEffectSwirlCircles}, {kGroupNoColor, kEffectMotionPoints},
    {kGroupNoColor, kEffectMarquee}, {kGroupNoColor, kEffectMosaic}, {kGroupNoColor, kEffectStarryNight},
    {kGroupNoColor, kEffectBubbles}, {kGroupNoColor, kEffectBloom}, {kGroupNoColor, kEffectSunrise},
    {kGroupNoColor, kEffectPolice},
};
static_assert(sizeof kEffectOrder / sizeof kEffectOrder[0] == kEffectCount, "every effect in a group, once");

const int kMaxNeon = 64;

// The four edges of the neon, in the order the lights go round the car (see edge_point). Each plays its own
// effect.
enum Zone { kZoneFront, kZoneRight, kZoneRear, kZoneLeft, kZoneCount };
const char* const kZoneNames[kZoneCount] = {"Front", "Right", "Rear", "Left"};
const char* const kZoneLabels[kZoneCount] = {"Front", "Right Side", "Rear", "Left Side"};
const char* const kZoneSettings[kZoneCount] = {"NeonEffectFront", "NeonEffectRight", "NeonEffectRear",
                                               "NeonEffectLeft"};
const wchar_t* const kZoneIniKeys[kZoneCount] = {L"FrontEffect", L"RightEffect", L"RearEffect", L"LeftEffect"};
// Each part on or off (only the sides, say): the Mods page's rows and the ini's keys.
const char* const kPartSettings[kZoneCount] = {"NeonFront", "NeonRight", "NeonRear", "NeonLeft"};
const wchar_t* const kPartIniKeys[kZoneCount] = {L"FrontOn", L"RightOn", L"RearOn", L"LeftOn"};

// The shape of the neon (ini [Neon], `tune` command).
// The lights go round a rectangle with a corner at each wheel: down both sides from the front wheel to the
// rear wheel, and across the front and the back between the wheels. The wheels are found from the car's own
// lights: the outermost of them gives the width, the headlights the front and the tail lights the back.
struct Shape {
    int side = 10;              // lights down each side, front wheel to rear wheel
    int front = 6;              // lights across the front, front left wheel to front right wheel
    int rear = 6;               // lights across the back, rear left wheel to rear right wheel
    float height = 0.2f;        // metres above the car's origin (about the ground)
    float front_inset = 0.9f;   // metres from the headlights back to the front wheels
    float rear_inset = 1.0f;    // metres from the tail lights forward to the rear wheels
    float side_offset = 0.1f;   // metres out from the outermost light to the wheels (negative: in)
    float side_trim = 0.45f;    // metres the side strips stop short of each wheel (clear of the wheel arches)
    float range = 3.0f;         // preset +0x60
    float power = 2.0f;         // preset +0x64
    float inner = 60.0f;      // preset +0x68, degrees
    float outer = 170.0f;     // preset +0x6C, degrees
};

struct ShapeField {
    const char* name;
    bool is_int;
    size_t offset;
};
const ShapeField kShapeFields[] = {
    {"Side", true, offsetof(Shape, side)},
    {"Front", true, offsetof(Shape, front)},
    {"Rear", true, offsetof(Shape, rear)},
    {"Height", false, offsetof(Shape, height)},
    {"FrontInset", false, offsetof(Shape, front_inset)},
    {"RearInset", false, offsetof(Shape, rear_inset)},
    {"SideOffset", false, offsetof(Shape, side_offset)},
    {"SideTrim", false, offsetof(Shape, side_trim)},
    {"Range", false, offsetof(Shape, range)},
    {"Power", false, offsetof(Shape, power)},
    {"Inner", false, offsetof(Shape, inner)},
    {"Outer", false, offsetof(Shape, outer)},
};

const int kMaxPlayerLights = 64;

struct Config {
    bool neon = true;
    int color = 0;                          // the primary color (a place in the menus' colors)
    int color2 = 3;                         // the secondary color (two-color effects): Pink
    float custom[kCustomCount][3] = {};     // the ini's custom colors
    char custom_name[kCustomCount][32] = {};   // their names ("" : Custom N)
    char custom_text[kCustomCount][32] = {};   // each as the ini has it, written back as it was
    float brightness = 1.0f;
    int effect[kZoneCount] = {kEffectStatic, kEffectStatic, kEffectStatic, kEffectStatic};
    bool part[kZoneCount] = {true, true, true, true};   // each part (zone) lit or not
    float speed = 0.3f;   // effect cycles a second
    Shape shape;
};

SRWLOCK g_cfg_lock = SRWLOCK_INIT;
Config g_cfg;

Config config() {
    AcquireSRWLockShared(&g_cfg_lock);
    Config c = g_cfg;
    ReleaseSRWLockShared(&g_cfg_lock);
    return c;
}

void color_rgb(const Config& c, int color, float out[3]) {
    const float* s = color >= 0 && color < kColorCount ? kColors[color].rgb
                     : color >= kColorCount && color < kColorTotal ? c.custom[color - kColorCount]
                                                                   : kColors[0].rgb;
    for (int i = 0; i < 3; ++i) out[i] = s[i];
}
void neon_rgb(const Config& c, float out[3]) { color_rgb(c, c.color, out); }
void second_rgb(const Config& c, float out[3]) { color_rgb(c, c.color2, out); }

// A color's English name, for the log and the commands: "Cyan", "Custom 3" or the ini's name for it.
const char* color_name(const Config& c, int color, char* buf, size_t size) {
    if (color >= 0 && color < kColorCount) return kColors[color].name;
    const int k = color - kColorCount;
    if (k >= 0 && k < kCustomCount && c.custom_name[k][0]) return c.custom_name[k];
    snprintf(buf, size, "Custom %d", k + 1);
    return buf;
}

void ini_write(const wchar_t* section, const wchar_t* key, const char* value) {
    wchar_t w[128];
    MultiByteToWideChar(CP_UTF8, 0, value, -1, w, 128);
    WritePrivateProfileStringW(section, key, w, g_ini);
}

float ini_float(const wchar_t* section, const wchar_t* key, float def) {
    wchar_t w[64];
    GetPrivateProfileStringW(section, key, L"", w, 64, g_ini);
    return w[0] ? (float)_wtof(w) : def;
}

int ini_int(const wchar_t* section, const wchar_t* key, int def) {
    wchar_t w[64];
    GetPrivateProfileStringW(section, key, L"", w, 64, g_ini);
    return w[0] ? (int)wcstol(w, nullptr, 0) : def;
}

int total_lights(const Shape& s) { return 2 * s.side + s.front + s.rear; }

void clamp_shape(Shape& s) {
    int* const edges[] = {&s.side, &s.front, &s.rear};
    for (int* n : edges) {
        if (*n < 0) *n = 0;
        if (*n > kMaxNeon) *n = kMaxNeon;
    }
    while (total_lights(s) > kMaxNeon) {   // trim the longest edges first
        if (s.side * 2 >= s.front && s.side * 2 >= s.rear && s.side) --s.side;
        else if (s.front >= s.rear && s.front) --s.front;
        else --s.rear;
    }
}

void load_config() {
    Config c;
    c.neon = ini_int(L"Settings", L"Neon", 1) != 0;
    // The custom colors: [CustomColors] Color1..10 ("r g b", 0 to 1) and Name1..10. Before them (0.4 and
    // older) there was one, [Settings] CustomColor, at 9 in the list: it's Custom 1 now.
    wchar_t w[96];
    GetPrivateProfileStringW(L"CustomColors", L"Color1", L"", w, 96, g_ini);
    const bool had_customs = w[0] != 0;
    for (int k = 0; k < kCustomCount; ++k) {
        memcpy(c.custom[k], kCustomDefaults[k], sizeof c.custom[k]);
        wchar_t key[16];
        swprintf_s(key, L"Color%d", k + 1);
        GetPrivateProfileStringW(L"CustomColors", key, L"", w, 96, g_ini);
        if (!w[0] && k == 0) GetPrivateProfileStringW(L"Settings", L"CustomColor", L"", w, 96, g_ini);
        char text[96];
        WideCharToMultiByte(CP_UTF8, 0, w, -1, text, sizeof text, nullptr, nullptr);
        float rgb[3];
        if (text[0] && parse_color(text, rgb)) {
            memcpy(c.custom[k], rgb, sizeof rgb);
            strncpy_s(c.custom_text[k], text, _TRUNCATE);
        }
        swprintf_s(key, L"Name%d", k + 1);
        GetPrivateProfileStringW(L"CustomColors", key, L"", w, 96, g_ini);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, c.custom_name[k], sizeof c.custom_name[k], nullptr, nullptr);
    }
    c.color = ini_int(L"Settings", L"Color", 0);
    c.color2 = ini_int(L"Settings", L"Color2", 3);
    if (!had_customs && c.color == 9) c.color = kColorCount;   // the old Custom
    if (!had_customs && c.color2 == 9) c.color2 = kColorCount;
    if (c.color < 0 || c.color >= kColorTotal) c.color = 0;
    if (c.color2 < 0 || c.color2 >= kColorTotal) c.color2 = 3;
    c.brightness = ini_float(L"Settings", L"Brightness", 1.0f);
    const int all = ini_int(L"Settings", L"Effect", kEffectStatic);   // 0.1's one effect for the whole car
    for (int z = 0; z < kZoneCount; ++z) {
        c.effect[z] = ini_int(L"Settings", kZoneIniKeys[z], all);
        c.part[z] = ini_int(L"Settings", kPartIniKeys[z], 1) != 0;
        if (c.effect[z] < 0 || c.effect[z] >= kEffectCount) c.effect[z] = kEffectStatic;
    }
    c.speed = ini_float(L"Settings", L"Speed", 0.3f);
    for (const auto& f : kShapeFields) {
        char* p = reinterpret_cast<char*>(&c.shape) + f.offset;
        wchar_t key[32];
        MultiByteToWideChar(CP_UTF8, 0, f.name, -1, key, 32);
        if (f.is_int) *reinterpret_cast<int*>(p) = ini_int(L"Neon", key, *reinterpret_cast<int*>(p));
        else *reinterpret_cast<float*>(p) = ini_float(L"Neon", key, *reinterpret_cast<float*>(p));
    }
    clamp_shape(c.shape);
    AcquireSRWLockExclusive(&g_cfg_lock);
    g_cfg = c;
    ReleaseSRWLockExclusive(&g_cfg_lock);
}

// The ini's last write time: the watcher reloads it when someone else changes it.
std::atomic<uint64_t> g_ini_time{0};

uint64_t ini_write_time() {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &a)) return 0;
    return (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
}

void save_config() {
    Config c = config();
    ini_write(L"Settings", L"Neon", c.neon ? "1" : "0");
    char b[64];
    snprintf(b, sizeof b, "%d", c.color);
    ini_write(L"Settings", L"Color", b);
    snprintf(b, sizeof b, "%d", c.color2);
    ini_write(L"Settings", L"Color2", b);
    WritePrivateProfileStringW(L"Settings", L"CustomColor", nullptr, g_ini);    // now [CustomColors] Color1
    WritePrivateProfileStringW(L"Settings", L"CustomColor2", nullptr, g_ini);   // 0.4's, never released
    for (int k = 0; k < kCustomCount; ++k) {
        wchar_t key[16];
        swprintf_s(key, L"Color%d", k + 1);
        if (c.custom_text[k][0]) strcpy_s(b, c.custom_text[k]);   // as it was written: "0 1 1", "255 0 0", "#FF0000"
        else
            snprintf(b, sizeof b, "#%02X%02X%02X", int(c.custom[k][0] * 255.0f + 0.5f), int(c.custom[k][1] * 255.0f + 0.5f),
                     int(c.custom[k][2] * 255.0f + 0.5f));
        ini_write(L"CustomColors", key, b);
        swprintf_s(key, L"Name%d", k + 1);
        ini_write(L"CustomColors", key, c.custom_name[k]);
    }
    snprintf(b, sizeof b, "%g", c.brightness);
    ini_write(L"Settings", L"Brightness", b);
    WritePrivateProfileStringW(L"Settings", L"Effect", nullptr, g_ini);   // replaced by the four below
    for (int z = 0; z < kZoneCount; ++z) {
        snprintf(b, sizeof b, "%d", c.effect[z]);
        ini_write(L"Settings", kZoneIniKeys[z], b);
        ini_write(L"Settings", kPartIniKeys[z], c.part[z] ? "1" : "0");
    }
    snprintf(b, sizeof b, "%g", c.speed);
    ini_write(L"Settings", L"Speed", b);
    for (const auto& f : kShapeFields) {
        const char* p = reinterpret_cast<const char*>(&c.shape) + f.offset;
        if (f.is_int) snprintf(b, sizeof b, "%d", *reinterpret_cast<const int*>(p));
        else snprintf(b, sizeof b, "%g", *reinterpret_cast<const float*>(p));
        wchar_t key[32];
        MultiByteToWideChar(CP_UTF8, 0, f.name, -1, key, 32);
        ini_write(L"Neon", key, b);
    }
    g_ini_time.store(ini_write_time());   // our own write isn't a change to reload
}

// The effects aren't on the Mods page (more than a choice row holds): ANNA, the ini and `effect` set them.
void publish_effects(const Config&) {}

// ---- The game's language ----------------------------------------------------------------------------------------
// As FH2 ANNA reads it: the string tables' manager ([game+0x3662990]) has the text language's code, an MSVC
// std::string ("EN", "DE", "CHT"...), at +0x30. The Mods page's rows are written before the game has a language,
// so the one seen last is kept in the ini ([Language] Last; FH2ANNA.ini's when Underglow has none yet).

const uintptr_t kStringTables = 0x3662990;
std::atomic<int> g_lang_saved{-2};   // an index into neon_text::kCodes, -1 English, -2 not read yet

// Czech and Hungarian: the game's own text is English there, and its fonts may not have their letters.
bool english_there(int i) { return !strcmp(neon_text::kCodes[i], "CZ") || !strcmp(neon_text::kCodes[i], "HU"); }

int lang_index(const wchar_t* code) {
    for (int i = 0; i < neon_text::kLangs; ++i) {
        wchar_t w[8];
        MultiByteToWideChar(CP_UTF8, 0, neon_text::kCodes[i], -1, w, 8);
        if (!_wcsicmp(code, w)) return english_there(i) ? -1 : i;
    }
    return -1;
}

void lang_load() {
    wchar_t code[8] = {};
    GetPrivateProfileStringW(L"Language", L"Last", L"", code, 8, g_ini);
    if (!code[0]) {
        wchar_t anna[MAX_PATH];
        swprintf_s(anna, L"%s\\FH2ANNA.ini", g_mods_dir);
        GetPrivateProfileStringW(L"Language", L"Last", L"EN", code, 8, anna);
    }
    g_lang_saved.store(lang_index(code));
}

int game_language() {
    uintptr_t strings = 0;
    uint8_t name[32];
    if (!g_base || !rdp(g_base + kStringTables, &strings) || !rd(strings + 0x30, name, sizeof name)) return -2;
    size_t count, capacity;
    memcpy(&count, name + 16, sizeof count);
    memcpy(&capacity, name + 24, sizeof capacity);
    if (capacity != 15 || count < 2 || count > 3 || name[count]) return -2;
    wchar_t code[8];
    MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(name), -1, code, 8);
    return lang_index(code);   // EN, GB, DEV, LOC: English
}

int language() {
    const int now = game_language();
    if (now == -2) return g_lang_saved.load() < -1 ? -1 : g_lang_saved.load();
    if (now != g_lang_saved.exchange(now)) {   // for the Mods page next time
        wchar_t code[8];
        MultiByteToWideChar(CP_UTF8, 0, now < 0 ? "EN" : neon_text::kCodes[now], -1, code, 8);
        WritePrivateProfileStringW(L"Language", L"Last", code, g_ini);
        g_ini_time.store(ini_write_time());   // not a change to reload
    }
    return now;
}

// `english` in the game's language now (itself when there's no translation).
const char* tr(const char* english) {
    const int l = language();
    if (l < 0 || !english) return english;
    for (const auto& t : neon_text::kTexts)
        if (!strcmp(t.en, english)) return t.t[l];
    return english;
}

// A color's name in the menus: translated, or the ini's own name for a custom one.
const char* color_label(const Config& c, int color, char* buf, size_t size) {
    if (color >= 0 && color < kColorCount) return tr(kColors[color].name);
    const int k = color - kColorCount;
    if (k >= 0 && k < kCustomCount && c.custom_name[k][0]) return c.custom_name[k];
    snprintf(buf, size, "%s %d", tr("Custom"), k + 1);
    return buf;
}

// Live tuning: Underglow.ini saved in an editor takes effect within half a second.
DWORD WINAPI watch_ini(LPVOID) {
    g_ini_time.store(ini_write_time());
    for (;;) {
        Sleep(500);
        uint64_t t = ini_write_time();
        if (!t || t == g_ini_time.load()) continue;
        Sleep(100);   // let the editor finish writing
        g_ini_time.store(ini_write_time());
        load_config();
        Config c = config();
        g_api->set_setting(g_mod, "Neon", c.neon ? 1 : 0);
        g_api->set_setting(g_mod, "NeonColor", c.color);
        g_api->set_setting(g_mod, "NeonColor2", c.color2);
        for (int z = 0; z < kZoneCount; ++z) g_api->set_setting(g_mod, kPartSettings[z], c.part[z] ? 1 : 0);
        publish_effects(c);
        g_api->log(g_mod, "Underglow.ini changed: reloaded");
    }
}

void set_part(int zone, bool on) {
    if (zone < 0 || zone >= kZoneCount) return;
    AcquireSRWLockExclusive(&g_cfg_lock);
    g_cfg.part[zone] = on;
    ReleaseSRWLockExclusive(&g_cfg_lock);
    if (g_api) g_api->set_setting(g_mod, kPartSettings[zone], on ? 1 : 0);
    save_config();
}

void set_neon(bool on) {
    AcquireSRWLockExclusive(&g_cfg_lock);
    g_cfg.neon = on;
    ReleaseSRWLockExclusive(&g_cfg_lock);
    if (g_api) g_api->set_setting(g_mod, "Neon", on ? 1 : 0);
    save_config();
}

// zone < 0 (or kZoneCount): all four.
void set_effect(int zone, int index) {
    AcquireSRWLockExclusive(&g_cfg_lock);
    for (int z = 0; z < kZoneCount; ++z)
        if (zone < 0 || zone >= kZoneCount || z == zone) g_cfg.effect[z] = index;
    ReleaseSRWLockExclusive(&g_cfg_lock);
    publish_effects(config());
    save_config();
}

// which 0: the primary color, 1: the secondary.
void set_color(int index, int which = 0) {
    AcquireSRWLockExclusive(&g_cfg_lock);
    (which ? g_cfg.color2 : g_cfg.color) = index;
    ReleaseSRWLockExclusive(&g_cfg_lock);
    if (g_api) g_api->set_setting(g_mod, which ? "NeonColor2" : "NeonColor", index);
    save_config();
}

// ---- The player's car -----------------------------------------------------------------------------------------

std::atomic<uintptr_t> g_player_pres{0};

// The effects' clock: the player's car's own update time, so they stop with the game (the pause menu, photo
// mode) instead of running on the wall clock. In microseconds, and when it last moved.
std::atomic<uint64_t> g_game_us{0};
std::atomic<uint64_t> g_game_moved{0};

bool note_car(void* car) {
    uintptr_t c = reinterpret_cast<uintptr_t>(car);
    int kind = -1;
    if (!rd(c + kForzaCarKindOffset, &kind, 4) || kind != 0) return false;
    uintptr_t pres = 0;
    if (!rdp(c + kForzaCarPresOffset, &pres)) return false;
    if (vtable_is(pres, &kDeferredPresVtable, 1) && !rdp(pres + kDeferredPresOffset, &pres)) return false;
    if (!vtable_is(pres, kCarPresVtables, 2)) return false;
    g_player_pres.store(pres);
    return true;
}

void detour_car_update(void* car, float dt, float b, uint32_t flag) {
    g_orig_car_update(car, dt, b, flag);
    if (note_car(car) && dt > 0.0f && dt < 0.25f) {
        g_game_us.fetch_add(uint64_t(double(dt) * 1e6), std::memory_order_relaxed);
        g_game_moved.store(GetTickCount64(), std::memory_order_relaxed);
    }
}

struct PlayerLight {
    uint32_t record;
    uint16_t type;
    uint32_t handle;
    uintptr_t instance;
};

struct Resolved {
    uintptr_t pres = 0, manager = 0, asset = 0;
    size_t instances = 0;
    int count = 0;
    PlayerLight light[kMaxPlayerLights];
};

bool resolve_player(const Kind& k, uintptr_t system, Resolved* r) {
    r->count = 0;
    r->pres = g_player_pres.load();
    uintptr_t context = 0, sys = 0;
    if (!r->pres || !vtable_is(r->pres, kCarPresVtables, 2) || !rdp(r->pres + kPresContext, &context) ||
        !rdp(context + kContextManager, &r->manager) || !rdp(r->manager + k.manager_slot, &sys) || sys != system ||
        !rdp(r->pres + kPresAsset, &r->asset))
        return false;
    Vec inst, map, rec;
    size_t map_count = 0, rec_count = 0;
    if (!read_vec(r->asset + kAssetInstances, 8, 512, &inst, &r->instances) ||
        !read_vec(r->manager + kManagerMapping, kMappingStride, kMaxRecords, &map, &map_count) ||
        !read_vec(system + k.records, kRecordStride, kMaxRecords, &rec, &rec_count))
        return false;
    for (size_t i = 0; i < r->instances && r->count < kMaxPlayerLights; ++i) {
        uintptr_t instance = 0;
        uint8_t data[0x54];
        if (!rdp(inst.begin + i * 8, &instance) || !rd(instance, data, sizeof data)) continue;
        uint16_t type;
        uint32_t handle, record = ~0u;
        memcpy(&type, data + 4, 2);
        memcpy(&handle, data + 0x50, 4);
        if (handle >= map_count || !rd(map.begin + handle * kMappingStride + k.mapping_field, &record, 4) ||
            record >= rec_count)
            continue;
        bool dup = false;
        for (int j = 0; j < r->count; ++j) dup |= r->light[j].record == record;
        if (!dup) r->light[r->count++] = {record, type, handle, instance};
    }
    return true;
}

// What the neon needs from the player's car, refreshed at every beam upload.
const int kMaxRefLights = 16;

struct Reference {
    bool valid = false;
    uintptr_t pres = 0, manager = 0, beams = 0, glows = 0;
    // The car's lights we can follow: the neon is placed from whichever the game has just moved (headlights
    // that are off aren't moved, so following only a headlight leaves the neon behind).
    int lights = 0;
    uint32_t handle[kMaxRefLights];
    float local_pos[kMaxRefLights][3], local_rot[kMaxRefLights][4];
    float half_width = 0.8f, front = 2.0f, rear = -2.0f;   // the outermost lights, on the car
    uint32_t template_handle = ~0u;   // the light whose key/index/preset the neon copies (the tail glow)
    uint32_t template_preset = ~0u;
    uint64_t tick = 0;
};
SRWLOCK g_ref_lock = SRWLOCK_INIT;
Reference g_ref;

void remember_player(const Resolved& r, uintptr_t beams) {
    Reference ref;
    ref.pres = r.pres;
    ref.manager = r.manager;
    ref.beams = beams;
    rdp(r.manager + kManagerGlows, &ref.glows);
    int tail = -1;
    for (int i = 0; i < r.count; ++i) {
        if (r.light[i].type == kTypeTailGlow && tail < 0) tail = i;
        if (ref.lights < kMaxRefLights && rd(r.light[i].instance + 0x10, ref.local_pos[ref.lights], 12) &&
            rd(r.light[i].instance + 0x20, ref.local_rot[ref.lights], 16) && finite(ref.local_pos[ref.lights], 3) &&
            finite(ref.local_rot[ref.lights], 4))
            ref.handle[ref.lights++] = r.light[i].handle;
    }
    if (tail < 0) tail = 0;
    for (int i = 0; i < ref.lights; ++i) {
        const float* lp = ref.local_pos[i];
        if (i == 0 || std::fabs(lp[0]) > ref.half_width) ref.half_width = std::fabs(lp[0]);
        if (i == 0 || lp[2] > ref.front) ref.front = lp[2];
        if (i == 0 || lp[2] < ref.rear) ref.rear = lp[2];
    }
    if (ref.lights) {
        ref.template_handle = r.light[tail].handle;
        uintptr_t rec = beam_record(beams, r.light[tail].record);
        ref.valid = rec && rd(rec + kRecPreset, &ref.template_preset, 4);
    }
    ref.tick = GetTickCount64();
    AcquireSRWLockExclusive(&g_ref_lock);
    g_ref = ref;
    ReleaseSRWLockExclusive(&g_ref_lock);
}

std::atomic<bool> g_neon_build{false};   // the neon wants lights made (on the creating thread)
SRWLOCK g_neon_lock = SRWLOCK_INIT;      // the neon is only touched by whoever holds it
void neon_build(const Reference& ref);

// ---- CreateLight: remember each handle's key and index --------------------------------------------------------

const uint32_t kMaxHandles = 1u << 16;
uint32_t g_handle_key[kMaxHandles];
uint32_t g_handle_index[kMaxHandles];
uintptr_t g_handle_manager[kMaxHandles];

uint32_t* detour_create(void* mgr, uint32_t* handle, uint32_t key, uint32_t index) {
    uint32_t* r = g_orig_create(mgr, handle, key, index);
    uint32_t h = ~0u;
    rd(reinterpret_cast<uintptr_t>(handle), &h, 4);
    if (h < kMaxHandles) {
        g_handle_key[h] = key;
        g_handle_index[h] = index;
        g_handle_manager[h] = reinterpret_cast<uintptr_t>(mgr);
    }
    if (g_neon_build.load() && TryAcquireSRWLockExclusive(&g_neon_lock)) {
        static Reference ref;   // only used under g_neon_lock
        AcquireSRWLockShared(&g_ref_lock);
        ref = g_ref;
        ReleaseSRWLockShared(&g_ref_lock);
        if (ref.valid && ref.manager == reinterpret_cast<uintptr_t>(mgr)) {
            g_neon_build.store(false);
            neon_build(ref);
        }
        ReleaseSRWLockExclusive(&g_neon_lock);
    }
    return r;
}

// ---- The radio: what the Audio Visualizer listens to ----------------------------------------------------------
//
// The game's sound is FMOD 1.03.05 (System::getVersion), linked into the exe with its API exported by name. Every
// station plays through one mixer bus, the channel group with the game's "Stereo Transceiver" DSP on it (it's
// what carries the radio to festival speakers; XboxMusicRadio routes its station onto it the same way). We put
// FMOD's own FFT DSP (FMOD_DSP_TYPE_FFT = 29 in this build, game+0x19F8B05) on that bus: it passes the sound
// through unchanged and gives its spectrum. Everything runs inside FMOD's own System::update, on the thread that
// already drives FMOD, so nothing here races the game's sound.

const uintptr_t kFmodUpdate = 0x199D510;           // System::update(system)
const uintptr_t kFmodMasterGroup = 0x1998C90;      // System::getMasterChannelGroup(system, ChannelGroup**)
const uintptr_t kFmodSoftwareFormat = 0x1999F70;   // System::getSoftwareFormat(system, int* rate, int*, int*)
const uintptr_t kFmodCreateDspByType = 0x1997360;  // System::createDSPByType(system, type, DSP**)
const uintptr_t kFmodNumGroups = 0x19A7050;        // ChannelGroup::getNumGroups(group, int*)
const uintptr_t kFmodGetGroup = 0x19A6C90;         // ChannelGroup::getGroup(group, int, ChannelGroup**)
const uintptr_t kFmodGroupName = 0x19A6DE0;        // ChannelGroup::getName(group, char*, int)
const uintptr_t kFmodNumDsps = 0x19A3230;          // ChannelControl::getNumDSPs(ctl, int*)
const uintptr_t kFmodGetDsp = 0x19A2860;           // ChannelControl::getDSP(ctl, int, DSP**)
const uintptr_t kFmodAddDsp = 0x19A16E0;           // ChannelControl::addDSP(ctl, int index, DSP*, DSPConnection**)
const uintptr_t kFmodDspInfo = 0x19A84C0;          // DSP::getInfo(dsp, char name[32], uint*, int*, int*, int*)
const uintptr_t kFmodSetActive = 0x19A9BE0;        // DSP::setActive(dsp, bool)
const uintptr_t kFmodParamInt = 0x19A94B0;         // DSP::getParameterInt(dsp, int, int*, char*, int)
const uintptr_t kFmodParamData = 0x19A9180;        // DSP::getParameterData(dsp, int, void**, uint*, char*, int)
const int kFmodDspFft = 29;
const int kFftWindowSize = 0, kFftSpectrum = 2;    // FMOD_DSP_FFT parameters

using FmodUpdateFn = int (*)(void* system);
using FmodPtrFn = int (*)(void* self, void** out);
using FmodIntFn = int (*)(void* self, int* out);
using FmodIndexFn = int (*)(void* self, int index, void** out);
using FmodNameFn = int (*)(void* group, char* name, int size);
using FmodFormatFn = int (*)(void* system, int* rate, int* mode, int* raw);
using FmodCreateFn = int (*)(void* system, int type, void** dsp);
using FmodAddDspFn = int (*)(void* ctl, int index, void* dsp, void** connection);
using FmodDspInfoFn = int (*)(void* dsp, char* name, unsigned* version, int* channels, int* w, int* h);
using FmodSetActiveFn = int (*)(void* dsp, bool active);
using FmodParamIntFn = int (*)(void* dsp, int index, int* value, char* text, int size);
using FmodParamDataFn = int (*)(void* dsp, int index, void** data, unsigned* length, char* text, int size);

FmodUpdateFn g_orig_fmod_update = nullptr;

template <typename F>
F fmod_fn(uintptr_t rva_) { return reinterpret_cast<F>(g_base + rva_); }

// The four bands the visualizer shows, one per zone: the rear the bass, the left side the low mids, the right
// side the high mids, the front the treble. 0..1, levelled to the music's loudness.
std::atomic<float> g_radio_level[kZoneCount];
std::atomic<uint64_t> g_radio_tick{0};   // when the levels were last worked out (0: never)
std::atomic<bool> g_radio_attached{false};   // the FFT is on the radio bus: no more made-up beat

struct RadioTap {
    void* system = nullptr;
    void* bus = nullptr;
    void* fft = nullptr;
    int rate = 48000;
    int window = 0;
    void* searched_system[4] = {};
    uint64_t searched_at[4] = {};
    bool off = false;            // FMOD faulted once: leave it alone
    // Each band levelled against its own recent peak (radio_measure).
    float peak[kZoneCount] = {};
    float level[kZoneCount] = {};
    uint64_t last = 0;
    uint64_t heard = 0;          // when a band last had sound in it
    char state[256] = "not looked for yet (the game's sound hasn't updated)";
};
RadioTap g_radio;              // only touched inside FMOD's update (and read, racily, by `status`)
SRWLOCK g_radio_lock = SRWLOCK_INIT;

void radio_state(const char* fmt, ...) {
    char b[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (strcmp(b, g_radio.state) == 0) return;
    AcquireSRWLockExclusive(&g_radio_lock);
    strncpy_s(g_radio.state, b, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_radio_lock);
    if (g_api) g_api->log(g_mod, b);
}

bool dsp_name(void* dsp, char name[33]) {
    unsigned version = 0;
    int ch = 0, w = 0, h = 0;
    memset(name, 0, 33);
    if (fmod_fn<FmodDspInfoFn>(kFmodDspInfo)(dsp, name, &version, &ch, &w, &h) != 0) return false;
    for (int i = 31; i >= 0 && (name[i] == ' ' || !name[i]); --i) name[i] = 0;   // built-ins pad with spaces
    return true;
}

// The index of the DSP named `want` in a group's chain, or -1.
int dsp_index(void* group, const char* want) {
    int n = 0;
    if (fmod_fn<FmodIntFn>(kFmodNumDsps)(group, &n) != 0) return -1;
    for (int i = 0; i < n && i < 32; ++i) {
        void* dsp = nullptr;
        char name[33];
        if (fmod_fn<FmodIndexFn>(kFmodGetDsp)(group, i, &dsp) == 0 && dsp && dsp_name(dsp, name) &&
            strcmp(name, want) == 0)
            return i;
    }
    return -1;
}

// Does this group carry the DSP named `want` (or the DSP `ptr`)?
bool group_has_dsp(void* group, const char* want, void* ptr) {
    int n = 0;
    if (fmod_fn<FmodIntFn>(kFmodNumDsps)(group, &n) != 0) return false;
    for (int i = 0; i < n && i < 32; ++i) {
        void* dsp = nullptr;
        char name[33];
        if (fmod_fn<FmodIndexFn>(kFmodGetDsp)(group, i, &dsp) != 0 || !dsp) continue;
        if (ptr && dsp == ptr) return true;
        if (want && dsp_name(dsp, name) && strcmp(name, want) == 0) return true;
    }
    return false;
}

// The radio bus: the group with the Stereo Transceiver, preferring one named like the radio if there are more.
void* find_radio_bus(void* system, char* names, size_t size) {
    void* master = nullptr;
    if (fmod_fn<FmodPtrFn>(kFmodMasterGroup)(system, &master) != 0 || !master) return nullptr;
    void* queue[256];
    int head = 0, tail = 0;
    queue[tail++] = master;
    void* best = nullptr;
    bool best_named = false;
    names[0] = 0;
    while (head < tail) {
        void* g = queue[head++];
        if (group_has_dsp(g, "Stereo Transceiver", nullptr)) {
            char name[64] = {};
            fmod_fn<FmodNameFn>(kFmodGroupName)(g, name, sizeof name - 1);
            const bool named = strstr(name, "adio") != nullptr;
            if (!best || (named && !best_named)) {
                best = g;
                best_named = named;
            }
            if (names[0]) strncat_s(names, size, ", ", _TRUNCATE);
            strncat_s(names, size, name[0] ? name : "(no name)", _TRUNCATE);
        }
        int n = 0;
        if (fmod_fn<FmodIntFn>(kFmodNumGroups)(g, &n) != 0) continue;
        for (int i = 0; i < n && tail < 256; ++i) {
            void* child = nullptr;
            if (fmod_fn<FmodIndexFn>(kFmodGetGroup)(g, i, &child) == 0 && child) queue[tail++] = child;
        }
    }
    return best;
}

// Finds the bus and puts the FFT on it, or checks it's still there (every 2 s).
void radio_attach(void* system) {
    char names[160];
    void* bus = find_radio_bus(system, names, sizeof names);
    if (!bus) {
        if (!g_radio.bus) radio_state("radio: no radio bus in this FMOD system yet (looked for the Stereo Transceiver)");
        return;
    }
    if (bus == g_radio.bus && g_radio.fft && group_has_dsp(bus, nullptr, g_radio.fft)) return;   // all well
    void* fft = nullptr;
    char name[33];
    if (fmod_fn<FmodCreateFn>(kFmodCreateDspByType)(system, kFmodDspFft, &fft) != 0 || !fft) {
        radio_state("radio: FMOD wouldn't make an FFT DSP");
        return;
    }
    if (!dsp_name(fft, name) || strcmp(name, "FMOD FFT") != 0) {
        radio_state("radio: DSP type %d is \"%s\", not FMOD FFT: this FMOD differs, no radio for the visualizer",
                    kFmodDspFft, name);
        return;   // leaked on purpose: releasing an unknown DSP is riskier than keeping it
    }
    // Just before the bus's fader in the chain (FMOD runs a chain from its last index to index 0, the head):
    // the radio bus sends its music out through the Stereo Transceiver and keeps its own fader at 0, so after
    // the fader there's only silence.
    const int at = dsp_index(bus, "FMOD Fader") + 1;   // 0 if there's no fader
    void* connection = nullptr;
    if (fmod_fn<FmodAddDspFn>(kFmodAddDsp)(bus, at, fft, &connection) != 0) {
        radio_state("radio: couldn't add the FFT to the radio bus");
        return;
    }
    fmod_fn<FmodSetActiveFn>(kFmodSetActive)(fft, true);
    int window = 0;
    fmod_fn<FmodParamIntFn>(kFmodParamInt)(fft, kFftWindowSize, &window, nullptr, 0);
    int rate = 0, mode = 0, raw = 0;
    if (fmod_fn<FmodFormatFn>(kFmodSoftwareFormat)(system, &rate, &mode, &raw) != 0 || rate < 8000) rate = 48000;
    g_radio.system = system;
    g_radio.bus = bus;
    g_radio.fft = fft;
    g_radio.rate = rate;
    g_radio.window = window;
    memset(g_radio.peak, 0, sizeof g_radio.peak);
    g_radio_attached.store(true);
    radio_state("radio: listening on the radio bus (%s), %d Hz, FFT window %d, at DSP %d", names, rate, window, at);
}

// The radio bus also carries the festival's stage music, so the FFT alone can't tell whether the car radio is on.
// RadioSystem (global game+0x365D410, vtable 0x2E76818) keeps its player at +0x48; the player's current station is
// a shared_ptr at +0x48 (its getter, vtable slot +0x20 at 0x1008430), null when SetStation got "StationOff".
const uintptr_t kRadioSystemPtr = 0x365D410;
// Xbox Music doesn't go through the player: SetStation starts it (0x1022B60) and sets RadioSystem+0x85, which it
// clears again for StationOff or any other station.
bool car_radio_on() {
    uintptr_t radio = 0, player = 0, station = 0;
    if (!rdp(g_base + kRadioSystemPtr, &radio)) return false;
    uint8_t xbox_music = 0;
    if (rd(radio + 0x85, &xbox_music, 1) && xbox_music) return true;
    return rdp(radio + 0x48, &player) && rdp(player + 0x48, &station);
}

// The bands from the FFT's spectrum, levelled, into g_radio_level.
void radio_measure() {
    void* data = nullptr;
    unsigned length = 0;
    const int rc = fmod_fn<FmodParamDataFn>(kFmodParamData)(g_radio.fft, kFftSpectrum, &data, &length, nullptr, 0);
    // FMOD 1.03's spectrum data is a plain float array, one channel (the bus mixed down), window / 2 bins from
    // 0 Hz up to half the sample rate; it leaves `length` at 0. (Later FMODs give an FMOD_DSP_PARAMETER_FFT.)
    const int count = g_radio.window >= 32 && g_radio.window <= 16384 ? g_radio.window / 2 : 1024;
    static float bins[8192];
    bool ok = rc == 0 && data && rd(reinterpret_cast<uintptr_t>(data), bins, size_t(count) * 4);
    for (int k = 0; ok && k < count; ++k) ok = std::isfinite(bins[k]) && bins[k] >= 0.0f && bins[k] < 1000.0f;
    if (!ok) {
        for (int z = 0; z < kZoneCount; ++z) g_radio_level[z].store(0.0f, std::memory_order_relaxed);
        g_radio_tick.store(GetTickCount64(), std::memory_order_release);
        return;
    }
    const float hz = float(g_radio.rate) / float(2 * count);
    const float bands[kZoneCount][2] = {
        {3000.0f, 12000.0f},   // front: treble
        {800.0f, 3000.0f},     // right side: high mids
        {30.0f, 160.0f},       // rear: bass
        {160.0f, 800.0f},      // left side: low mids
    };
    float raw[kZoneCount];
    for (int z = 0; z < kZoneCount; ++z) {
        int lo = int(bands[z][0] / hz), hi = int(bands[z][1] / hz);
        if (lo < 1) lo = 1;
        if (hi > count - 1) hi = count - 1;
        if (hi < lo) hi = lo;
        float sum = 0.0f;
        for (int k = lo; k <= hi; ++k) sum += bins[k];
        raw[z] = sum / float(hi - lo + 1);
    }

    // FMOD 1.03 gives one near-silent spectrum (always the same values) every 8192 samples while music plays.
    // A silent frame only counts once the quiet has lasted a while; until then, keep the last reading.
    const uint64_t now = GetTickCount64();
    float loudest = 0.0f;
    for (float r : raw) loudest = r > loudest ? r : loudest;
    const bool radio_on = car_radio_on();
    const bool sound = radio_on && loudest > 1e-7f;
    if (!radio_on) g_radio.heard = 0;   // off is off at once, no dropout grace
    if (sound) {
        g_radio.heard = now;
    } else if (g_radio.heard && now - g_radio.heard < 300) {
        g_radio_tick.store(now, std::memory_order_release);
        return;
    } else {
        // Radio off still leaves a faint residue (~1e-9). Make it 0, or the slowly falling peak would catch down
        // to it after a minute and light the zones up at full.
        for (float& r : raw) r = 0.0f;
    }

    // Levelled: each band against its own recent peak (so the radio's volume doesn't matter), quick to rise and
    // easing back down. Silence stays dark.
    const float dt = g_radio.last ? float(now - g_radio.last) / 1000.0f : 0.016f;
    g_radio.last = now;
    const float peak_fall = std::exp(-dt * 0.25f), level_fall = std::exp(-dt * 7.0f);
    for (int z = 0; z < kZoneCount; ++z) {
        float& peak = g_radio.peak[z];
        peak = raw[z] > peak ? raw[z] : peak * peak_fall;
        float v = peak > 1e-8f ? raw[z] / peak : 0.0f;   // silence is exactly 0
        v = (v - 0.15f) / 0.85f;
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        float& lvl = g_radio.level[z];
        lvl = v > lvl ? v : lvl * level_fall + v * (1.0f - level_fall);
        g_radio_level[z].store(lvl, std::memory_order_relaxed);
    }
    g_radio_tick.store(now, std::memory_order_release);
}

// Every 2 s for each FMOD system (the game may have more than one), until one has the radio bus; then only it.
int radio_slot(void* system) {
    for (int i = 0; i < 4; ++i)
        if (g_radio.searched_system[i] == system) return i;
    for (int i = 0; i < 4; ++i)
        if (!g_radio.searched_system[i]) {
            g_radio.searched_system[i] = system;
            return i;
        }
    return 3;
}

bool radio_search_due(void* system, int slot, uint64_t now) {
    if (g_radio.fft && system != g_radio.system) return false;
    if (g_radio.searched_at[slot] && now - g_radio.searched_at[slot] < 2000) return false;
    g_radio.searched_at[slot] = now;
    return true;
}

int detour_fmod_update(void* system) {
    const int r = g_orig_fmod_update(system);
    if (g_radio.off) return r;
    __try {
        const int slot = radio_slot(system);
        if (radio_search_due(system, slot, GetTickCount64())) radio_attach(system);
        if (g_radio.fft && system == g_radio.system) radio_measure();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_radio.off = true;
        g_radio.fft = nullptr;
        g_radio_attached.store(false);
        radio_state("radio: FMOD faulted while Underglow read it; the visualizer is simulated from now on");
    }
    return r;
}

// The radio's level for a zone now, or -1 when Underglow isn't listening to the radio at all.
// Once Underglow listens to the radio bus, a radio that's off (or a game that's paused) is dark, never made up.
float radio_level(int zone) {
    if (!g_radio_attached.load()) return -1.0f;
    const uint64_t t = g_radio_tick.load(std::memory_order_acquire);
    if (!t || GetTickCount64() - t > 500) return 0.0f;
    return g_radio_level[zone].load(std::memory_order_relaxed);
}

// ---- The neon -------------------------------------------------------------------------------------------------

struct Neon {
    uintptr_t manager = 0, beams = 0;
    int count = 0;
    uint32_t handle[kMaxNeon];
    uint32_t preset[kMaxNeon];   // each light has a preset of its own, so each can have its own color
    float rgb[kMaxNeon][3];      // the color last written to it
    uint32_t spare = ~0u;        // a preset allocated for a light the game then didn't give us
    uint64_t shape_sig = 0;
    bool lit = false;
    uint64_t last_frame = 0;
    char problem[160] = {};
};
Neon g_neon;

void neon_problem(const char* text) {
    if (strcmp(g_neon.problem, text) == 0) return;
    strncpy_s(g_neon.problem, text, _TRUNCATE);
    if (g_api) g_api->log(g_mod, text);
}

void set_dirty() {
    const uint8_t one = 1;
    wr(g_neon.beams + kBeamDirty, &one, 1);
}

// One of our presets: the template's (the tail light's: wide, no headlight pattern), in the neon's shape and
// `rgb`, always on.
bool write_preset(const Shape& s, uint32_t preset, uint32_t template_preset, const float rgb[3]) {
    uintptr_t ours = beam_preset(g_neon.beams, preset);
    uintptr_t tmpl = beam_preset(g_neon.beams, template_preset);
    uint8_t b[kBeamPresetStride], keep[8];
    if (!ours || !tmpl || !rd(tmpl, b, sizeof b) || !rd(ours + kPreUsers, keep, 8)) return false;
    const float zero[2] = {0.0f, 0.0f};
    memcpy(b + kPreUsers, keep, 8);   // our users and GPU slot, not the template's
    memcpy(b + kPreRgb, rgb, 12);
    memcpy(b + kPreRange, &s.range, 4);
    memcpy(b + kPrePower, &s.power, 4);
    memcpy(b + kPreInner, &s.inner, 4);
    memcpy(b + kPreOuter, &s.outer, 4);
    memcpy(b + kPreThreshold, zero, 8);
    return wr(ours + 0x10, b + 0x10, sizeof b - 0x10);   // not the pool links at +0
}

bool our_light_ok(int i) {
    uint32_t beam, glow, preset = ~0u;
    uintptr_t rec;
    return mapping_of(g_neon.manager, g_neon.handle[i], &beam, &glow) && (rec = beam_record(g_neon.beams, beam)) &&
           rd(rec + kRecPreset, &preset, 4) && preset == g_neon.preset[i];
}

void neon_dark() {
    for (int i = 0; i < g_neon.count; ++i)   // the glow sprites too
        g_orig_set_int(reinterpret_cast<void*>(g_neon.manager), g_neon.handle[i], 0.0f);
    g_neon.lit = false;
}

void glow_off(uintptr_t glows, uint32_t handle) {
    uint32_t beam, glow;
    if (glows && mapping_of(g_neon.manager, handle, &beam, &glow) && glow != ~0u)
        reinterpret_cast<GlowIntensityFn>(g_base + kGlowSetIntensity)(reinterpret_cast<void*>(glows), glow, 0.0f);
}

// Creates the neon's lights, each with a preset of its own. Runs on the thread the game creates its own lights
// on, right after one of its CreateLight calls, so it never races the game's own creating (traffic spawning).
void neon_build(const Reference& ref) {
    const Config c = config();
    void* mgr = reinterpret_cast<void*>(ref.manager);
    if (g_neon.manager != ref.manager || g_neon.beams != ref.beams) return;   // neon_frame resets it first
    if (ref.template_handle >= kMaxHandles || g_handle_manager[ref.template_handle] != ref.manager) {
        neon_problem("not creating the neon yet: the car's lights were made before Underglow was watching "
                     "(change car or fast travel once)");
        return;
    }
    float rgb[3];
    neon_rgb(c, rgb);
    while (g_neon.count < total_lights(c.shape)) {
        uint32_t p = g_neon.spare;
        g_neon.spare = ~0u;
        if (p == ~0u) {
            p = reinterpret_cast<AllocPresetFn>(g_base + kAllocBeamPreset)(
                reinterpret_cast<void*>(ref.beams + kBeamPresetPool));
            if (p == ~0u || !beam_preset(ref.beams, p)) {
                neon_problem("the game has no free light preset for the neon");
                break;
            }
            const uint32_t no_users[2] = {0, 0};
            wr(beam_preset(ref.beams, p) + kPreUsers, no_users, 8);
        }
        if (!write_preset(c.shape, p, ref.template_preset, rgb)) {
            g_neon.spare = p;
            neon_problem("couldn't write a neon preset");
            break;
        }
        uint32_t h = ~0u, beam, glow;
        g_orig_create(mgr, &h, g_handle_key[ref.template_handle], g_handle_index[ref.template_handle]);
        if (h == ~0u || !mapping_of(ref.manager, h, &beam, &glow) || beam == ~0u) {
            g_neon.spare = p;
            neon_problem("CreateLight gave the neon no light");
            break;
        }
        reinterpret_cast<AttachPresetFn>(g_base + kAttachPreset)(reinterpret_cast<void*>(ref.beams), beam, p);
        const int i = g_neon.count++;
        g_neon.handle[i] = h;
        g_neon.preset[i] = p;
        memcpy(g_neon.rgb[i], rgb, sizeof rgb);
    }
    g_neon.shape_sig = fnv(&c.shape, sizeof c.shape);
    set_dirty();
}

// Light i of the wheel rectangle, on the car: the front edge (front left wheel to front right), the right side
// (front right to rear right), the back (rear right to rear left), then the left side (rear left to front
// left). Each edge's lights sit in the middle of equal stretches of it, so none doubles up at a corner.
void edge_point(const Reference& ref, const Shape& s, int i, float out[3]) {
    float hw = ref.half_width + s.side_offset;
    if (hw < 0.1f) hw = 0.1f;
    float zf = ref.front - s.front_inset, zr = ref.rear + s.rear_inset;
    if (zf < zr + 0.2f) zf = zr = (zf + zr) * 0.5f;
    const float corner[4][2] = {{-hw, zf}, {hw, zf}, {hw, zr}, {-hw, zr}};
    const int per_edge[4] = {s.front, s.side, s.rear, s.side};
    int e = 0;
    while (e < 3 && i >= per_edge[e]) i -= per_edge[e++];
    const float t = per_edge[e] > 0 ? (float(i) + 0.5f) / float(per_edge[e]) : 0.5f;
    float a[2] = {corner[e][0], corner[e][1]};
    float b[2] = {corner[(e + 1) % 4][0], corner[(e + 1) % 4][1]};
    if (e == 1 || e == 3) {   // the sides: only between the wheel arches, as a real strip is fitted
        float trim = s.side_trim, len = zf - zr;
        if (trim < 0.0f) trim = 0.0f;
        if (trim > len * 0.45f) trim = len * 0.45f;
        const float dir = b[1] > a[1] ? 1.0f : -1.0f;
        a[1] += dir * trim;
        b[1] -= dir * trim;
    }
    out[0] = a[0] + (b[0] - a[0]) * t;
    out[1] = s.height;
    out[2] = a[1] + (b[1] - a[1]) * t;
}

// A fully saturated color at hue h (0..1: red, yellow, green, cyan, blue, magenta, red).
void hue_rgb(float h, float out[3]) {
    h = (h - std::floor(h)) * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    const int sector = int(h) % 6;
    const float table[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
    memcpy(out, table[sector], 12);
}

// Where light i of the wheel rectangle is, for the effects: its zone, and its place along that zone counted the
// same way on both sides of the car (front and rear: left to right; the sides: front to back).
struct Pixel {
    int zone = 0;
    int li = 0, ln = 1;       // index and count in its zone
    float t = 0.5f;           // (li + 0.5) / ln
    float lateral = 0.0f;     // -1 the car's left .. +1 its right
    int gi = 0, gn = 1;       // index and count along every zone playing the same effect, round the car
    float ring = 0.5f;        // (gi + 0.5) / gn
    // Where it is on the car, for the effects drawn on it as a picture (a wheel, a spiral, bubbles...):
    float x = 0.0f, y = 0.0f; // -1 the left .. +1 the right; +1 the front .. -1 the back
    float ang = 0.0f;         // round the car from the middle: 0 the front, 0.25 the right, 0.5 the back, 0.75 the left
    float rd = 0.0f;          // 0 the middle of the sides .. 1 the front and back
    int id = 0;               // the light's own number, for the random ones
};

void zone_of(const Shape& s, int i, Pixel* p) {
    const int per_edge[kZoneCount] = {s.front, s.side, s.rear, s.side};
    int e = 0;
    while (e < kZoneCount - 1 && i >= per_edge[e]) i -= per_edge[e++];
    p->zone = e;
    p->ln = per_edge[e] > 0 ? per_edge[e] : 1;
    // edge_point runs the rear right to left and the left side back to front: turn those round.
    p->li = e == kZoneRear || e == kZoneLeft ? p->ln - 1 - i : i;
    p->t = (float(p->li) + 0.5f) / float(p->ln);
    p->lateral = e == kZoneRight ? 1.0f : e == kZoneLeft ? -1.0f : 2.0f * p->t - 1.0f;
    p->x = p->lateral;
    p->y = e == kZoneFront ? 1.0f : e == kZoneRear ? -1.0f : 1.0f - 2.0f * p->t;
    p->ang = float(std::atan2(double(p->x), double(p->y)) / 6.283185307179586);
    if (p->ang < 0.0f) p->ang += 1.0f;
    p->rd = std::sqrt(0.16f * p->x * p->x + p->y * p->y) / 1.0770330f;
    if (p->rd > 1.0f) p->rd = 1.0f;
}

double frac(double x) { return x - std::floor(x); }
float clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
float tri(double x) { return float(1.0 - std::fabs(2.0 * frac(x) - 1.0)); }   // 0 -> 1 -> 0 over a cycle

float hash01(uint32_t x) {   // a repeatable "random" number in 0..1
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return float(x & 0xFFFFFF) / float(0x1000000);
}

// The music, once a frame, for the audio effects: each zone's level (0..1) and a peak that holds, then falls.
struct AudioFrame {
    float lvl[kZoneCount] = {};
    float peak[kZoneCount] = {};
    uint64_t peak_at[kZoneCount] = {};
    uint64_t last = 0;
};
AudioFrame g_audio;   // only touched on the game's light thread (neon_frame)

void audio_frame(const Config& c, double secs) {
    const uint64_t now = GetTickCount64();
    const float dt = g_audio.last ? float(now - g_audio.last) / 1000.0f : 0.016f;
    g_audio.last = now;
    for (int z = 0; z < kZoneCount; ++z) {
        // The radio's music: the rear the bass, the left the low mids, the right the high mids, the front the treble.
        // Only when Underglow has no radio at all (FMOD not hooked, the bus not found yet): a made-up beat.
        float lvl = radio_level(z);
        if (lvl < 0.0f) {
            const double a = secs * (1.0 + 6.0 * c.speed);
            const double zz = double(z) * 1.7;
            const double beat = std::pow(1.0 - frac(a * 0.5), 3.0);
            lvl = clamp01(float(0.3 + 0.2 * std::sin(a * 2.3 + zz) + 0.15 * std::sin(a * 5.1 + 2.0 * zz) +
                                0.1 * std::sin(a * 9.7 + 3.0 * zz) + 0.35 * beat));
        }
        g_audio.lvl[z] = lvl;
        float& pk = g_audio.peak[z];
        if (lvl >= pk) {
            pk = lvl;
            g_audio.peak_at[z] = now;
        } else if (now - g_audio.peak_at[z] > 400) {   // held for 0.4 s, then falls
            pk -= 0.8f * dt;
            if (pk < lvl) pk = lvl;
        }
    }
}

bool silent(float lvl) { return lvl <= 0.03f; }

// How far apart two places round the car are (0..0.5 of the way round).
float adist(float a, float b) { return std::fabs(float(frac(double(a) - double(b) + 0.5)) - 0.5f); }
float smooth01(float x) {
    x = clamp01(x);
    return x * x * (3.0f - 2.0f * x);
}

void mix_rgb(const float a[3], const float b[3], float t, float out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = a[k] + (b[k] - a[k]) * t;
}

// A soft dot at `pos` (0..1 along the zone), `width` lights wide each way.
float dot(const Pixel& p, float pos, float width) {
    const float d = std::fabs((float(p.li) + 0.5f) - pos * float(p.ln));
    return clamp01(1.0f - d / width);
}

// Light p's color and intensity (0..1 of the brightness) now, playing `effect`.
void effect_now(const Config& c, int effect, const Pixel& p, double secs, float rgb[3], float* level) {
    const double phase = secs * c.speed;
    const double kTau = 6.283185307179586;
    float base[3], other[3];
    neon_rgb(c, base);
    second_rgb(c, other);
    memcpy(rgb, base, 12);
    *level = 1.0f;
    switch (effect) {
    case kEffectRainbow:
        hue_rgb(float(phase), rgb);
        break;
    case kEffectWave:
        hue_rgb(float(phase + p.ring), rgb);
        break;
    case kEffectBreathe:
        *level = float(0.1 + 0.9 * (0.5 - 0.5 * std::cos(phase * kTau)));
        break;
    case kEffectWipe: {   // the color wipes on along the zone, then off the same way
        const int step = int(frac(phase) * 2.0 * p.ln);
        *level = step < p.ln ? (p.li <= step ? 1.0f : 0.0f) : (p.li > step - p.ln ? 1.0f : 0.0f);
        break;
    }
    case kEffectStack: {   // lights drop one at a time from the start and pile up at the end
        const int n = p.ln;
        const int moves = n * (n + 1) / 2, hold = n > 2 ? n : 2;
        int step = int(frac(phase * 0.5) * double(moves + hold));
        int stacked = 0;
        while (stacked < n && step >= n - stacked) step -= n - stacked++;
        *level = p.li >= n - stacked || (stacked < n && p.li == step) ? 1.0f : 0.0f;
        break;
    }
    case kEffectBlink:
        *level = frac(phase * 5.0) < 0.5 ? 1.0f : 0.0f;
        break;
    case kEffectPolice: {   // left red, right blue, a double flash each in turn
        const double q = frac(phase * 3.0);
        const bool left = p.lateral <= 0.0f;
        const float red[3] = {1.0f, 0.0f, 0.0f}, blue[3] = {0.0f, 0.15f, 1.0f};
        memcpy(rgb, left ? red : blue, 12);
        const double h = frac(q * 2.0);
        const bool turn = left == (q < 0.5);
        *level = turn && (h < 0.2 || (h > 0.3 && h < 0.5)) ? 1.0f : 0.0f;
        break;
    }
    case kEffectAudio: {   // a level meter from the middle of each zone outwards, green to red
        const float lvl = g_audio.lvl[p.zone];
        const float d = std::fabs(2.0f * p.t - 1.0f);   // 0 the middle, 1 the ends
        hue_rgb((1.0f - d) / 3.0f, rgb);                  // green in the middle, red at the ends
        *level = !silent(lvl) ? clamp01((lvl - d) * float(p.ln) * 0.5f + 1.0f) : 0.0f;   // silence: dark
        break;
    }
    case kEffectAudioPulse: {   // the whole zone in the primary color, as bright as its band is loud
        const float lvl = g_audio.lvl[p.zone];
        *level = silent(lvl) ? 0.0f : lvl;
        break;
    }
    case kEffectAudioPeak: {   // a bar up the zone in the primary color, a falling peak light in the secondary
        const float lvl = g_audio.lvl[p.zone], pk = g_audio.peak[p.zone];
        if (silent(pk)) {
            *level = 0.0f;
            break;
        }
        int top = int(pk * float(p.ln));
        if (top > p.ln - 1) top = p.ln - 1;
        if (p.li == top) {
            memcpy(rgb, other, 12);
            *level = 1.0f;
        } else {
            *level = silent(lvl) ? 0.0f : clamp01(lvl * float(p.ln) - float(p.li));
        }
        break;
    }
    case kEffectAudioBlend: {   // quiet: the primary color, dim; loud: the secondary, bright
        const float lvl = g_audio.lvl[p.zone];
        mix_rgb(base, other, lvl, rgb);
        *level = silent(lvl) ? 0.0f : 0.2f + 0.8f * lvl;
        break;
    }
    case kEffectBassFlash: {   // the whole car flashes on the bass, primary; the hardest hits in the secondary
        const float bass = g_audio.lvl[kZoneRear];
        if (bass > 0.85f) memcpy(rgb, other, 12);
        *level = silent(bass) ? 0.0f : bass * bass;
        break;
    }
    case kEffectAudioSparkle: {   // lights twinkle at random in the primary color, more of them the louder it is
        const float lvl = g_audio.lvl[p.zone];
        const double slots = secs * 12.0;
        const uint32_t slot = uint32_t(int64_t(slots));
        const float f = float(frac(slots));
        const float r = hash01(slot * 131u + uint32_t(p.gi) * 7u + uint32_t(p.zone));
        *level = !silent(lvl) && r < lvl * 0.7f ? (1.0f - f) * (1.0f - f) : 0.0f;
        break;
    }
    case kEffectAudioCenter: {   // from the middle outwards with the music, primary in the middle to secondary
        const float lvl = g_audio.lvl[p.zone];
        const float d = std::fabs(2.0f * p.t - 1.0f);
        mix_rgb(base, other, d, rgb);
        *level = !silent(lvl) ? clamp01((lvl - d) * float(p.ln) * 0.5f + 1.0f) : 0.0f;
        break;
    }
    case kEffectAudioRainbow: {   // a rainbow running round, each zone as bright as its band; the beat pushes the hue
        const float lvl = g_audio.lvl[p.zone];
        hue_rgb(float(phase + p.ring) + 0.3f * lvl, rgb);
        *level = silent(lvl) ? 0.0f : 0.15f + 0.85f * lvl;
        break;
    }
    case kEffectCrossing: {   // two beams from opposite ends, crossing in the middle
        const float pos = tri(phase * 1.5);
        const float a = dot(p, pos, 1.5f), b = dot(p, 1.0f - pos, 1.5f);
        const float sum = a + b > 0.0f ? a + b : 1.0f;
        for (int k = 0; k < 3; ++k) rgb[k] = (base[k] * a + other[k] * b) / sum;
        *level = a > b ? a : b;
        break;
    }
    case kEffectHypnotoad: {   // rainbow rings running out from the middle, pulsing
        const float d = std::fabs(2.0f * p.t - 1.0f);
        hue_rgb(float(d * 1.5 - phase * 2.0), rgb);
        *level = float(0.25 + 0.75 * (0.5 + 0.5 * std::sin(phase * kTau * 3.0 - d * 6.0)));
        break;
    }
    case kEffectLightning: {   // now and then, a few quick flashes on a random stretch
        const double slots = secs * (0.5 + 2.0 * c.speed);
        const uint32_t slot = uint32_t(int64_t(slots)) * 4u + uint32_t(p.zone);
        const double into = frac(slots) / (0.5 + 2.0 * c.speed);   // seconds into the slot
        *level = 0.0f;
        if (hash01(slot * 7u + 1u) > 0.55f) break;   // no strike this time
        const double start = hash01(slot * 7u + 2u) * 0.4;
        const int flashes = 1 + int(hash01(slot * 7u + 3u) * 3.0f);
        const int from = int(hash01(slot * 7u + 4u) * float(p.ln));
        const int len = 1 + int(hash01(slot * 7u + 5u) * float(p.ln));
        if (p.li < from || p.li >= from + len) break;
        for (int f = 0; f < flashes; ++f) {
            const double t0 = start + f * 0.1;
            if (into >= t0 && into < t0 + 0.05) *level = f == 0 ? 1.0f : 0.6f;
        }
        break;
    }
    case kEffectMarquee: {   // every third light on, running round, each a random color
        const int64_t step = int64_t(phase * 20.0);
        const int64_t k = int64_t(p.gi) - step;
        const int64_t m = ((k % 3) + 3) % 3;
        const int64_t group = (k - m) / 3;
        hue_rgb(hash01(uint32_t(group)), rgb);
        *level = m == 0 ? 1.0f : 0.0f;
        break;
    }
    case kEffectComet: {   // a bright head running round the car, a fading tail behind it
        const double head = frac(phase * 1.5);
        const double behind = frac(head - p.ring);
        const double tail = std::fmax(0.3, 4.0 / double(p.gn));
        *level = behind < tail ? float((1.0 - behind / tail) * (1.0 - behind / tail)) : 0.0f;
        break;
    }
    case kEffectFill: {   // the zone fills up from its start, then empties back
        const float lvl = tri(phase);
        *level = clamp01(lvl * float(p.ln) - float(p.li));
        break;
    }
    case kEffectSwap: {   // half one color, half the other, swapping over
        const bool first = p.t < 0.5f;
        const bool swapped = int64_t(phase * 2.0) % 2 != 0;
        if (first == swapped) memcpy(rgb, other, 12);
        break;
    }
    case kEffectVisor: {   // one light sweeping back and forth, scanner style
        const float pos = float(0.5 - 0.5 * std::cos(phase * 1.5 * kTau));
        const float w = std::fmax(1.5f, float(p.ln) * 0.2f);
        const float v = dot(p, pos, w);
        *level = v * v;
        break;
    }

    // ---- 0.5: effects after OpenRGB Effects Plugin's, written afresh for a ring of lights round a car ----
    // The music: each part's band (g_audio), as for the audio effects above.
    case kEffectAudioBubbles: {   // rings of color pop up round the car with the music and spread
        const float lvl = g_audio.lvl[p.zone];
        *level = 0.0f;
        if (silent(lvl)) break;
        const double slots = secs * 5.0;
        const int64_t now = int64_t(std::floor(slots));
        for (int64_t k = now; k > now - 4; --k) {
            const uint32_t id = uint32_t(k);
            const float age = float((slots - double(k)) / 4.0);
            const float r = 0.02f + age * 0.15f * (0.5f + lvl);
            const float v = clamp01(1.0f - std::fabs(adist(p.ang, hash01(id * 13u + 1u)) - r) / 0.035f) * (1.0f - age);
            if (v > *level) {
                *level = v;
                hue_rgb(hash01(id * 13u + 2u), rgb);
            }
        }
        *level = clamp01(*level * (0.4f + lvl));
        break;
    }
    case kEffectAudioParty: {   // each part flashes with its band, in a color that keeps changing
        const float lvl = g_audio.lvl[p.zone];
        hue_rgb(hash01(uint32_t(int64_t(secs * 3.0)) * 4u + uint32_t(p.zone)), rgb);
        *level = silent(lvl) ? 0.0f : lvl * lvl;
        break;
    }
    case kEffectAudioSine: {   // a sine wave running along each part, as high as the music is loud
        const float lvl = g_audio.lvl[p.zone];
        const float w = float(0.5 + 0.5 * std::sin((p.t * 2.0 - phase * 2.0) * kTau));
        *level = silent(lvl) ? 0.0f : clamp01(w * lvl * 1.6f);
        break;
    }
    case kEffectAudioStar: {   // colored stars come out with the music, more of them the louder, and fade
        const float lvl = g_audio.lvl[p.zone];
        *level = 0.0f;
        if (silent(lvl)) break;
        const double slots = secs * 4.0;
        const int64_t now = int64_t(std::floor(slots));
        for (int64_t k = now; k > now - 2; --k) {
            const uint32_t h = uint32_t(k) * 97u + uint32_t(p.id) * 31u;
            if (hash01(h + 5u) >= lvl * 0.5f) continue;
            const float v = 1.0f - float((slots - double(k)) / 2.0);
            if (v > *level) {
                *level = v;
                hue_rgb(hash01(h + 6u), rgb);
            }
        }
        break;
    }
    case kEffectAudioSync: {   // the whole car one color: red with the bass, through yellow and green, blue with treble
        const float b = g_audio.lvl[kZoneRear], lm = g_audio.lvl[kZoneLeft], hm = g_audio.lvl[kZoneRight],
                    tr = g_audio.lvl[kZoneFront];
        const float sum = b + lm + hm + tr, loud = std::fmax(std::fmax(b, lm), std::fmax(hm, tr));
        hue_rgb(sum > 0.0f ? (lm * 0.15f + hm * 0.4f + tr * 0.65f) / sum : 0.0f, rgb);
        *level = silent(loud) ? 0.0f : loud;
        break;
    }
    case kEffectVuMeter: {   // a bar up each part with its band: the primary color, to the secondary at the top
        const float lvl = g_audio.lvl[p.zone];
        mix_rgb(base, other, p.t, rgb);
        *level = silent(lvl) ? 0.0f : clamp01(lvl * float(p.ln) - float(p.li));
        break;
    }
    case kEffectAudioSwirl: {   // two lights, the two colors, chasing round the car, as big as the music is loud
        const float loud = (g_audio.lvl[0] + g_audio.lvl[1] + g_audio.lvl[2] + g_audio.lvl[3]) * 0.25f;
        const float a = float(frac(phase * 0.7)), b = float(frac(phase * 0.7 + 0.5));
        const float w = 0.04f + 0.2f * loud;
        const float va = clamp01(1.0f - adist(p.ang, a) / w), vb = clamp01(1.0f - adist(p.ang, b) / w);
        memcpy(rgb, va >= vb ? base : other, 12);   // one in the primary color, one in the secondary
        *level = silent(loud) ? 0.0f : std::fmax(va, vb);
        break;
    }
    // One color: the primary.
    case kEffectBouncingBall: {   // a ball bouncing along each part, slowing at the top of each bounce
        const double u = frac(phase * 1.5);
        *level = dot(p, float(4.0 * u * (1.0 - u)), 1.3f);
        break;
    }
    case kEffectBreathingCircle: {   // a ring of light growing out from the middle of the car and back
        const float r = float(0.5 - 0.5 * std::cos(phase * kTau));
        *level = clamp01(1.0f - std::fabs(p.rd - r) / 0.25f);
        break;
    }
    case kEffectFractalMotion: {   // waves over waves of light, never quite the same twice
        const double a = p.ang * kTau, q = phase * 3.0;
        const double v = std::sin(a * 2.0 + q * 1.1) * 0.5 + std::sin(a * 5.0 - q * 1.7) * 0.3 +
                         std::sin(a * 9.0 + q * 2.3) * 0.2;
        *level = clamp01(float(0.5 + 0.6 * v));
        break;
    }
    case kEffectMotionPoint: {   // one light gliding round the car, easing to a stop, and back
        const float pos = float(0.5 - 0.5 * std::cos(phase * 0.5 * kTau));
        *level = clamp01(1.0f - adist(p.ang, pos) / 0.05f);
        break;
    }
    case kEffectRain: {   // drops running along each part, front to back down the sides
        *level = 0.0f;
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t d = uint32_t(p.zone) * 3u + k;
            const double run = phase * (1.5 + hash01(d * 7u + 1u)) + hash01(d * 7u + 2u);
            const float behind = float(frac(run)) * 1.3f - p.t;   // a gap off the end between drops
            if (behind >= 0.0f && behind < 0.25f) *level = std::fmax(*level, 1.0f - behind / 0.25f);
        }
        break;
    }
    case kEffectRotatingBeam: {   // a beam through the middle of the car, turning
        const float a = float(frac(phase * 0.5));
        *level = clamp01(1.0f - std::fmin(adist(p.ang, a), adist(p.ang, a + 0.5f)) / 0.07f);
        break;
    }
    case kEffectSmoothBlink: {   // on, a soft fade out, off, a soft fade in
        const float u = float(frac(phase * 2.0));
        *level = u < 0.4f ? 1.0f : u < 0.5f ? smooth01((0.5f - u) * 10.0f) : u < 0.9f ? 0.0f : smooth01((u - 0.9f) * 10.0f);
        break;
    }
    case kEffectSparkleFade: {   // lights flick on here and there and fade away
        const double slots = phase * 8.0 + hash01(uint32_t(p.id) * 3u + 1u);
        const float f = float(frac(slots));
        *level = hash01(uint32_t(int64_t(slots)) * 131u + uint32_t(p.id) * 17u) < 0.3f ? (1.0f - f) * (1.0f - f) : 0.0f;
        break;
    }
    case kEffectZigZag: {   // a light zigzagging from side to side down the car
        const double u = frac(phase * 0.5);
        const float py = float(1.0 - 2.0 * u), qx = 2.0f * tri(u * 3.0) - 1.0f;
        const float d = std::sqrt((p.x - qx) * (p.x - qx) + (p.y - py) * (p.y - py));
        *level = clamp01(1.0f - d / 0.35f);
        break;
    }
    case kEffectMarquee1: {   // every third light on, running round
        const int64_t k = int64_t(p.gi) - int64_t(phase * 20.0);
        *level = ((k % 3) + 3) % 3 == 0 ? 1.0f : 0.0f;
        break;
    }
    // Two colors: the primary and the secondary.
    case kEffectAlternateBlink: {   // blinks in the primary color, then in the secondary
        const double u = frac(phase * 2.5);
        if (u >= 0.5) memcpy(rgb, other, 12);
        *level = frac(u * 2.0) < 0.6 ? 1.0f : 0.0f;
        break;
    }
    case kEffectGradientWave: {   // the primary color fading into the secondary and back, running round
        mix_rgb(base, other, float(0.5 - 0.5 * std::cos((p.ring - phase) * kTau)), rgb);
        break;
    }
    case kEffectMovingPanes: {   // panes of the two colors, neighbouring parts sliding opposite ways
        const double dir = p.zone == kZoneFront || p.zone == kZoneRight ? 1.0 : -1.0;
        const int64_t pane = int64_t(std::floor(p.t * 4.0 + dir * phase * 2.0));
        if (((pane % 2) + 2) % 2) memcpy(rgb, other, 12);
        break;
    }
    case kEffectNoiseMap: {   // the two colors drifting into each other in slow clouds
        const double a = p.ang * kTau, q = phase * 3.0;
        const double n = std::sin(a * 2.0 + q) * 0.6 + std::sin(a * 3.0 - q * 0.7 + 1.3) * 0.4;
        mix_rgb(base, other, smooth01(float(0.5 + 0.5 * n)), rgb);
        break;
    }
    case kEffectSequence: {   // the whole car in the primary color, then the secondary, fading over
        const float u = float(frac(phase));
        const float m = u < 0.4f ? 0.0f : u < 0.5f ? smooth01((u - 0.4f) * 10.0f) : u < 0.9f ? 1.0f : smooth01((1.0f - u) * 10.0f);
        mix_rgb(base, other, m, rgb);
        break;
    }
    case kEffectWavy: {   // a standing wave, each light swinging between the two colors
        const double w = std::sin(p.t * 2.0 * kTau) * std::sin(phase * kTau);
        mix_rgb(base, other, float(0.5 + 0.5 * w), rgb);
        break;
    }
    case kEffectTwoColorMarquee: {   // pairs of lights, the two colors in turn, running round
        const int64_t k = int64_t(p.gi) - int64_t(phase * 20.0);
        const int64_t g = int64_t(std::floor(double(k) / 3.0));
        if (g % 2 != 0) memcpy(rgb, other, 12);
        *level = ((k % 3) + 3) % 3 != 2 ? 1.0f : 0.0f;
        break;
    }
    // No colors: their own.
    case kEffectBloom:     // flowers of color opening round the car and fading
    case kEffectBubbles: {   // rings of color spreading round the car
        const bool ring = effect == kEffectBubbles;
        *level = 0.0f;
        const double slots = phase * 3.0;
        const int64_t now = int64_t(std::floor(slots));
        for (int64_t k = now; k > now - 3; --k) {
            const uint32_t id = uint32_t(k) * (ring ? 7u : 11u);
            const float age = float((slots - double(k)) / 3.0);
            const float d = adist(p.ang, hash01(id + 1u)), r = 0.02f + age * 0.15f;
            const float v = ring ? clamp01(1.0f - std::fabs(d - r) / 0.03f) * (1.0f - age)
                                 : d < r ? (1.0f - age) * (1.0f - 0.5f * d / r) : 0.0f;
            if (v > *level) {
                *level = v;
                hue_rgb(hash01(id + 2u) + (ring ? 0.0f : d * 2.0f), rgb);
            }
        }
        break;
    }
    case kEffectColorWheel:   // the color wheel round the car, turning
        hue_rgb(p.ang + float(phase), rgb);
        break;
    case kEffectRotatingRainbow: {   // a rainbow straight across the car, turning round
        const double a = phase * 0.5 * kTau;
        hue_rgb(float((p.x * std::cos(a) + p.y * std::sin(a)) * 0.35), rgb);
        break;
    }
    case kEffectDoubleRainbow:   // two rainbows turning opposite ways, meeting at the front and the back
        hue_rgb((p.x >= 0.0f ? 2.0f * p.ang : -2.0f * p.ang) + float(phase), rgb);
        break;
    case kEffectRadialRainbow:   // rainbow rings running out from the middle of the car
        hue_rgb(p.rd - float(phase), rgb);
        break;
    case kEffectSpiral:   // a rainbow spiral, turning
        hue_rgb(p.ang + p.rd * 1.5f - float(phase), rgb);
        break;
    case kEffectRandomSpin: {   // each part's rainbow spinning, its speed and way changing every few seconds
        const uint32_t k = uint32_t(int64_t(phase * 0.8)) * 4u + uint32_t(p.zone);
        hue_rgb(p.t + float(phase * (hash01(k * 3u + 1u) * 2.0f - 1.0f) * 3.0), rgb);
        break;
    }
    case kEffectSwirlCircles: {   // two colors chasing each other round the car
        const float a = float(frac(phase * 0.6)), b = float(frac(phase * 0.6 + 0.5));
        const float va = clamp01(1.0f - adist(p.ang, a) / 0.2f), vb = clamp01(1.0f - adist(p.ang, b) / 0.2f);
        hue_rgb(float(phase * 0.15) + (va >= vb ? 0.0f : 0.5f), rgb);
        *level = std::fmax(va, vb);
        break;
    }
    case kEffectMotionPoints: {   // several lights gliding round, each its own color, speed and way
        *level = 0.0f;
        for (uint32_t k = 0; k < 5; ++k) {
            const float sp = (0.3f + 0.7f * hash01(k * 5u + 1u)) * (hash01(k * 5u + 4u) < 0.5f ? -1.0f : 1.0f);
            const float v = clamp01(1.0f - adist(p.ang, float(frac(phase * sp + hash01(k * 5u + 2u)))) / 0.04f);
            if (v > *level) {
                *level = v;
                hue_rgb(hash01(k * 5u + 3u), rgb);
            }
        }
        break;
    }
    case kEffectMosaic: {   // tiles of two lights, each fading over to a new color now and then
        const uint32_t tile = uint32_t(p.id / 2);
        const double run = phase * 1.5 + hash01(tile * 7u + 1u) * 4.0;
        hue_rgb(hash01(uint32_t(int64_t(run)) * 101u + tile * 7u), rgb);
        *level = float(std::sin(frac(run) * 3.141592653589793));
        break;
    }
    case kEffectStarryNight: {   // stars coming out and fading, here and there, each its own color
        const double run = phase * 1.2 + hash01(uint32_t(p.id) * 9u + 1u) * 7.0;
        const uint32_t h = uint32_t(int64_t(run)) * 61u + uint32_t(p.id) * 13u;
        hue_rgb(hash01(h + 2u), rgb);
        *level = hash01(h) < 0.45f ? float(std::sin(frac(run) * 3.141592653589793)) : 0.0f;
        break;
    }
    case kEffectSunrise: {   // a deep red glow from the middle, warming to gold and white, then again
        const float u = float(frac(phase * 0.25));
        const float glow = clamp01(u * 1.6f - p.rd * 0.6f);
        hue_rgb(0.0f + 0.13f * glow, rgb);
        const float white = clamp01((glow - 0.7f) / 0.3f) * 0.7f;
        for (int k = 0; k < 3; ++k) rgb[k] += (1.0f - rgb[k]) * white;
        *level = u > 0.92f ? glow * (1.0f - u) / 0.08f : glow;
        break;
    }
    default:
        break;
    }
}

// Runs on the game's light thread, each frame, right after it updated one of the player's lights (it does
// that for every light, on or off).

void neon_frame(const Reference& ref) {
    const Config c = config();
    void* mgr = reinterpret_cast<void*>(ref.manager);

    if (g_neon.manager != ref.manager || g_neon.beams != ref.beams) {   // another light manager: start over
        g_neon = Neon{};
        g_neon.manager = ref.manager;
        g_neon.beams = ref.beams;
    }
    g_neon.last_frame = GetTickCount64();
    if (!c.neon) {
        if (g_neon.lit) neon_dark();
        return;
    }

    // Lights the game took back (their records no longer use our preset) are forgotten.
    int kept = 0;
    for (int i = 0; i < g_neon.count; ++i) {
        if (our_light_ok(i)) {
            g_neon.handle[kept] = g_neon.handle[i];
            g_neon.preset[kept] = g_neon.preset[i];
            memcpy(g_neon.rgb[kept], g_neon.rgb[i], 12);
            ++kept;
        }
    }
    g_neon.count = kept;

    // Creating lights is left to the game's own light-creating thread (see detour_create).
    const int want = total_lights(c.shape);
    if (g_neon.count < want) g_neon_build.store(true);
    if (!g_neon.count) return;

    // The car's transform: its presentation's world matrix, which the game places its own lights from.
    alignas(16) float world[16];
    if (!vtable_is(ref.pres, kCarPresVtables, 2) || !rd(ref.pres + kPresWorld, world, sizeof world) ||
        !finite(world, 16))
        return;
    alignas(16) float car_rot[4];
    _mm_store_ps(car_rot, reinterpret_cast<MatrixToQuatFn>(g_base + kMatrixToQuat)(world));
    if (!finite(car_rot, 4)) return;
    const float car_pos[3] = {world[12], world[13], world[14]};

    const uint64_t shape_sig = fnv(&c.shape, sizeof c.shape);
    const bool reshape = shape_sig != g_neon.shape_sig;
    bool dirty = false;
    // The game's time, not the wall clock's: paused (the pause menu, photo mode), the effects hold still.
    const uint64_t now_ms = GetTickCount64();
    const double secs = double(g_game_us.load(std::memory_order_relaxed) % 86400000000ull) / 1e6;
    const bool paused = now_ms - g_game_moved.load(std::memory_order_relaxed) > 150;
    if (!paused) audio_frame(c, secs);   // paused: the music's lights hold too

    const float down[4] = {0.70710678f, 0.0f, 0.0f, 0.70710678f};   // the light's +z turned to -y
    alignas(16) float rot[4];   // the game loads these with vmovaps: 16-byte aligned or it faults
    qmul(car_rot, down, rot);
    const int n = g_neon.count < want ? g_neon.count : want;

    // Each light's zone, and its place among all the zones playing the same effect (so a comet or a wave runs
    // on from one into the next).
    static Pixel px[kMaxNeon];
    // Parts switched off are left out, so the effect runs on through the ones that are on.
    int group_count[kEffectCount] = {};
    for (int i = 0; i < want; ++i) {
        zone_of(c.shape, i, &px[i]);
        px[i].id = i;
        px[i].gi = c.part[px[i].zone] ? group_count[c.effect[px[i].zone]]++ : 0;
    }
    for (int i = 0; i < want; ++i) {
        const int gn = group_count[c.effect[px[i].zone]];
        px[i].gn = gn > 0 ? gn : 1;
        px[i].ring = (float(px[i].gi) + 0.5f) / float(px[i].gn);
    }

    for (int i = 0; i < g_neon.count; ++i) {
        const uint32_t h = g_neon.handle[i];
        if (i >= n) {
            g_orig_set_int(mgr, h, 0.0f);
            continue;
        }
        float rgb[3], level;
        effect_now(c, c.effect[px[i].zone], px[i], secs, rgb, &level);
        if (!c.part[px[i].zone]) level = 0.0f;   // this part is switched off
        const float intensity = level > 0.01f ? c.brightness * level : 0.0f;
        if (intensity == 0.0f && !reshape) memcpy(rgb, g_neon.rgb[i], 12);   // dark: no need to recolor it
        if (reshape) {
            dirty |= write_preset(c.shape, g_neon.preset[i], ref.template_preset, rgb);
            memcpy(g_neon.rgb[i], rgb, 12);
        } else if (std::fabs(rgb[0] - g_neon.rgb[i][0]) + std::fabs(rgb[1] - g_neon.rgb[i][1]) +
                       std::fabs(rgb[2] - g_neon.rgb[i][2]) > 0.002f) {
            uintptr_t p = beam_preset(g_neon.beams, g_neon.preset[i]);
            if (p && wr(p + kPreRgb, rgb, 12)) dirty = true;
            memcpy(g_neon.rgb[i], rgb, 12);
        }

        float local[3];
        edge_point(ref, c.shape, i, local);
        float w[3];
        qrot(car_rot, local, w);
        alignas(16) const float pos[4] = {car_pos[0] + w[0], car_pos[1] + w[1], car_pos[2] + w[2], 1.0f};
        g_orig_set_pos(mgr, h, pos);
        g_orig_set_dir(mgr, h, rot);
        g_orig_set_int(mgr, h, intensity);
        glow_off(ref.glows, h);
    }
    if (reshape) g_neon.shape_sig = shape_sig;
    if (dirty) set_dirty();   // the presets go to the GPU again (the game only uploads them when told)
    g_neon.lit = true;
    g_neon.problem[0] = 0;
}

void detour_set_int(void* mgr, uint32_t handle, float v) {
    g_orig_set_int(mgr, handle, v);

    // The game updates each of the player's lights once a frame and ends with this, on or off: place the neon.
    static Reference ref;   // only used under g_neon_lock
    if (!TryAcquireSRWLockExclusive(&g_neon_lock)) return;
    AcquireSRWLockShared(&g_ref_lock);
    bool mine = g_ref.valid && reinterpret_cast<uintptr_t>(mgr) == g_ref.manager;
    if (mine) ref = g_ref;
    ReleaseSRWLockShared(&g_ref_lock);
    bool player_light = false;
    for (int k = 0; mine && k < ref.lights; ++k) player_light |= ref.handle[k] == handle;
    if (player_light) neon_frame(ref);
    ReleaseSRWLockExclusive(&g_neon_lock);
}

// ---- The preset upload: the player's car, and the neon dark when it stops -------------------------------------

SRWLOCK g_render_lock = SRWLOCK_INIT;

void upload(const Kind& k, UploadFn orig, void* system_p, void* graphics) {
    const uintptr_t system = reinterpret_cast<uintptr_t>(system_p);
    if (!TryAcquireSRWLockExclusive(&g_render_lock)) {
        orig(system_p, graphics);
        return;
    }
    static Resolved r;
    const bool mine = resolve_player(k, system, &r);
    if (mine && &k == &kBeam) remember_player(r, system);

    // The neon goes dark when the game stops updating the car (menus, garage, loading).
    if (&k == &kBeam && TryAcquireSRWLockExclusive(&g_neon_lock)) {
        if (g_neon.lit && GetTickCount64() - g_neon.last_frame > 250 && g_neon.beams == system) neon_dark();
        ReleaseSRWLockExclusive(&g_neon_lock);
    }

    orig(system_p, graphics);
    ReleaseSRWLockExclusive(&g_render_lock);
}

void detour_beam_upload(void* system, void* graphics) { upload(kBeam, g_orig_beam, system, graphics); }

// ---- Commands -------------------------------------------------------------------------------------------------

void cmd_status(char* reply, size_t size) {
    Config c = config();
    float rgb[3];
    char name1[48], name2[48];
    neon_rgb(c, rgb);
    AcquireSRWLockShared(&g_ref_lock);
    Reference ref = g_ref;
    ReleaseSRWLockShared(&g_ref_lock);
    AcquireSRWLockExclusive(&g_neon_lock);
    Neon n = g_neon;
    ReleaseSRWLockExclusive(&g_neon_lock);
    char radio[400];
    AcquireSRWLockShared(&g_radio_lock);
    int at = snprintf(radio, sizeof radio, "%s", g_radio.state);
    ReleaseSRWLockShared(&g_radio_lock);
    if (radio_level(0) >= 0.0f)
        snprintf(radio + at, sizeof radio - at, "; levels now: bass %.2f, low mids %.2f, high mids %.2f, treble %.2f",
                 radio_level(kZoneRear), radio_level(kZoneLeft), radio_level(kZoneRight), radio_level(kZoneFront));
    else
        snprintf(radio + at, sizeof radio - at, "; not listening to the radio (the Audio Visualizer plays a made-up beat)");
    const Shape& s = c.shape;
    snprintf(reply, size,
             "neon %s, primary color %s (%g %g %g), secondary %s, brightness %g, effects: front %s, right %s, rear %s, left %s, at %g a second; parts on: %s%s%s%s\n"
             "shape: Side %d Front %d Rear %d Height %g FrontInset %g RearInset %g SideOffset %g SideTrim %g Range %g Power %g Inner %g Outer %g\n"
             "car (from its lights): half width %.2f, front %.2f, rear %.2f\n"
             "neon: %d light(s), %s\n"
             "%s\n%s",
             c.neon ? "on" : "off", color_name(c, c.color, name1, sizeof name1), rgb[0], rgb[1], rgb[2],
             color_name(c, c.color2, name2, sizeof name2),
             c.brightness, kEffects[c.effect[kZoneFront]], kEffects[c.effect[kZoneRight]],
             kEffects[c.effect[kZoneRear]], kEffects[c.effect[kZoneLeft]], c.speed,
             c.part[kZoneFront] ? "front " : "", c.part[kZoneRight] ? "right " : "", c.part[kZoneRear] ? "rear " : "",
             c.part[kZoneLeft] ? "left" : "", s.side, s.front, s.rear, s.height, s.front_inset, s.rear_inset, s.side_offset, s.side_trim, s.range, s.power, s.inner,
             s.outer, ref.half_width, ref.front, ref.rear, n.count, n.lit ? "lit" : "dark", radio, n.problem);
}

void cmd_tune(const char* args, char* reply, size_t size) {
    char name[32] = {}, value[32] = {};
    if (sscanf_s(args, "%31s %31s", name, (unsigned)sizeof name, value, (unsigned)sizeof value) == 2) {
        bool found = false;
        AcquireSRWLockExclusive(&g_cfg_lock);
        for (const auto& f : kShapeFields) {
            if (_stricmp(f.name, name) != 0) continue;
            char* p = reinterpret_cast<char*>(&g_cfg.shape) + f.offset;
            if (f.is_int) *reinterpret_cast<int*>(p) = (int)strtol(value, nullptr, 0);
            else *reinterpret_cast<float*>(p) = strtof(value, nullptr);
            clamp_shape(g_cfg.shape);
            found = true;
        }
        ReleaseSRWLockExclusive(&g_cfg_lock);
        if (!found) {
            snprintf(reply, size, "error: tune <Side|Front|Rear|Height|FrontInset|RearInset|SideOffset|SideTrim|Range|Power|Inner|Outer> <value>");
            return;
        }
        save_config();
    }
    cmd_status(reply, size);
}

// "front Comet" -> kZoneFront, "Comet". No zone (or "all"): -1.
const char* parse_zone(const char* args, int* zone) {
    *zone = -1;
    const size_t len = strcspn(args, " ");
    for (int z = 0; z < kZoneCount; ++z)
        if (len == strlen(kZoneNames[z]) && _strnicmp(args, kZoneNames[z], len) == 0) *zone = z;
    if (*zone < 0 && !(len == 3 && _strnicmp(args, "all", 3) == 0)) return args;
    args += len;
    while (*args == ' ') ++args;
    return args;
}

// An effect by number or by name, case and spaces ignored ("crossingbeams", "Audio Visualizer", "wave").
int parse_effect(const char* text) {
    char* end = nullptr;
    long n = strtol(text, &end, 10);
    if (end != text && !*end) return n >= 0 && n < kEffectCount ? int(n) : -1;
    auto squash = [](const char* s, char* out, size_t size) {
        size_t o = 0;
        for (; *s && o + 1 < size; ++s)
            if (*s != ' ') out[o++] = char(tolower((unsigned char)*s));
        out[o] = 0;
    };
    char want[64], name[64];
    squash(text, want, sizeof want);
    if (!want[0]) return -1;
    if (strcmp(want, "wave") == 0) return kEffectWave;
    if (strcmp(want, "audio") == 0) return kEffectAudio;
    if (strcmp(want, "crossing") == 0) return kEffectCrossing;
    for (int i = 0; i < kEffectCount; ++i) {
        squash(kEffects[i], name, sizeof name);
        if (strcmp(want, name) == 0) return i;
    }
    return -1;
}

void command(void*, const char* name, const char* args, char* reply, size_t size) {
    if (_stricmp(name, "status") == 0) cmd_status(reply, size);
    else if (_stricmp(name, "tune") == 0) cmd_tune(args, reply, size);
    else if (_stricmp(name, "neon") == 0) {
        if (_stricmp(args, "on") == 0 || _stricmp(args, "off") == 0) set_neon(_stricmp(args, "on") == 0);
        else if (args[0]) {
            snprintf(reply, size, "error: neon on|off");
            return;
        }
        cmd_status(reply, size);
    } else if (_stricmp(name, "color") == 0 || _stricmp(name, "color2") == 0) {
        const int which = _stricmp(name, "color2") == 0 ? 1 : 0;
        float rgb[3];
        int found = -1, slot = 0;
        char squashed[64] = {};
        for (int i = 0, o = 0; args[i] && o < 63; ++i)
            if (args[i] != ' ') squashed[o++] = args[i];
        for (int i = 0; i < kColorCount; ++i) {
            char n[64] = {};
            for (int j = 0, o = 0; kColors[i].name[j] && o < 63; ++j)
                if (kColors[i].name[j] != ' ') n[o++] = kColors[i].name[j];
            if (_stricmp(squashed, n) == 0) found = i;
        }
        const Config now = config();
        for (int k = 0; k < kCustomCount; ++k)
            if (now.custom_name[k][0] && _stricmp(args, now.custom_name[k]) == 0) found = kColorCount + k;
        if (found < 0 && _strnicmp(squashed, "custom", 6) == 0 && sscanf_s(squashed + 6, "%d", &slot) == 1 &&
            slot >= 1 && slot <= kCustomCount)
            found = kColorCount + slot - 1;
        if (found >= 0) set_color(found, which);
        else if (parse_color(args, rgb)) {
            // The custom color in use, or Custom 1.
            const int in_use = which ? now.color2 : now.color;
            const int k = in_use >= kColorCount ? in_use - kColorCount : 0;
            AcquireSRWLockExclusive(&g_cfg_lock);
            memcpy(g_cfg.custom[k], rgb, sizeof rgb);
            g_cfg.custom_text[k][0] = 0;   // saved as hex
            ReleaseSRWLockExclusive(&g_cfg_lock);
            set_color(kColorCount + k, which);
        } else {
            char list[512] = {};
            for (int i = 0; i < kColorCount; ++i) {
                strcat_s(list, i ? "|" : "");
                strcat_s(list, kColors[i].name);
            }
            snprintf(reply, size,
                     "error: %s <%s|Custom1..Custom%d> | %s <r g b (0-1 or 0-255) | #RRGGBB> (sets the custom color in use)",
                     name, list, kCustomCount, name);
            return;
        }
        cmd_status(reply, size);
    } else if (_stricmp(name, "effect") == 0) {
        int zone = -1, found = -1;
        const char* rest = parse_zone(args, &zone);
        found = parse_effect(rest);
        if (found < 0) {
            char list[512] = {};
            for (int i = 0; i < kEffectCount; ++i) {
                strcat_s(list, i ? "|" : "");
                strcat_s(list, kEffects[i]);
            }
            snprintf(reply, size, "error: effect [front|right|rear|left|all] <%s> (or 0-%d)", list, kEffectCount - 1);
            return;
        }
        set_effect(zone, found);
        cmd_status(reply, size);
    } else if (_stricmp(name, "part") == 0) {
        int zone = -1;
        const char* rest = parse_zone(args, &zone);
        while (*rest == ' ') ++rest;
        const bool on = _stricmp(rest, "on") == 0, off = _stricmp(rest, "off") == 0;
        if (!on && !off) {
            snprintf(reply, size, "error: part [front|right|rear|left|all] on|off");
            return;
        }
        for (int z = 0; z < kZoneCount; ++z)
            if (zone < 0 || zone >= kZoneCount || z == zone) set_part(z, on);
        cmd_status(reply, size);
    } else if (_stricmp(name, "speed") == 0) {
        char* end = nullptr;
        float v = strtof(args, &end);
        if (end == args || v < 0.0f) {
            snprintf(reply, size, "error: speed <cycles a second>");
            return;
        }
        AcquireSRWLockExclusive(&g_cfg_lock);
        g_cfg.speed = v;
        ReleaseSRWLockExclusive(&g_cfg_lock);
        save_config();
        cmd_status(reply, size);
    } else if (_stricmp(name, "brightness") == 0) {
        char* end = nullptr;
        float b = strtof(args, &end);
        if (end == args || b < 0.0f) {
            snprintf(reply, size, "error: brightness <x>");
            return;
        }
        AcquireSRWLockExclusive(&g_cfg_lock);
        g_cfg.brightness = b;
        ReleaseSRWLockExclusive(&g_cfg_lock);
        save_config();
        cmd_status(reply, size);
    }
}

void add_commands() {
    struct {
        const char* name;
        const char* help;
    } const cmds[] = {
        {"status", "status: the neon, its lights and the player's car"},
        {"neon", "neon on|off"},
        {"color", "color <name|Custom1..Custom10> | color <r g b | #RRGGBB>: the primary color"},
        {"color2", "color2 <name|Custom1..Custom10> | color2 <r g b | #RRGGBB>: the secondary color (two-color effects)"},
        {"brightness", "brightness <x>: the neon lights' intensity"},
        {"effect", "effect [front|right|rear|left|all] <name|number>: Static, Rainbow, Rainbow Wave, Breathe, "
                   "Wipe, Stack, Blink, Police, Audio Visualizer, Crossing Beams, Hypnotoad, Lightning, "
                   "Random Marquee, Comet, Fill, Swap, Visor, Audio Pulse, Peak Meter, Audio Blend, Bass Flash, "
                   "Audio Sparkle, Audio Rainbow, Center Burst and more (the README lists all 61)"},
        {"part", "part [front|right|rear|left|all] on|off: switch a part of the underglow"},
        {"speed", "speed <x>: effect cycles a second"},
        {"tune", "tune <Side|Front|Rear|Height|FrontInset|RearInset|SideOffset|SideTrim|Range|Power|Inner|Outer> <value> (saved to the ini)"},
    };
    for (const auto& c : cmds) g_api->add_command(g_mod, c.name, c.help, command, nullptr);
}

// ---- Mods page and ANNA ---------------------------------------------------------------------------------------

const char* g_color_choices[kColorTotal];
char g_color_choice_text[kColorTotal][64];
const char* g_zone_effect_choices[kEffectCount];

void on_setting(void*, const char* key, int value) {
    AcquireSRWLockExclusive(&g_cfg_lock);
    if (strcmp(key, "Neon") == 0) g_cfg.neon = value != 0;
    else if (strcmp(key, "NeonColor") == 0 && value >= 0 && value < kColorTotal) g_cfg.color = value;
    else if (strcmp(key, "NeonColor2") == 0 && value >= 0 && value < kColorTotal) g_cfg.color2 = value;
    for (int z = 0; z < kZoneCount; ++z)
        if (strcmp(key, kPartSettings[z]) == 0) {
            g_cfg.part[z] = value != 0;
            ReleaseSRWLockExclusive(&g_cfg_lock);
            save_config();
            return;
        }
    ReleaseSRWLockExclusive(&g_cfg_lock);
    save_config();
}

void add_settings() {
    Config c = config();
    FH2Setting s{};
    s.size = sizeof s;
    s.key = "Neon";
    s.label = tr("Underglow");
    s.description = tr("Neon lights under your car. Also in ANNA: Car > Lights and Car > Underglow.");
    s.value = c.neon ? 1 : 0;
    s.on_change = on_setting;
    g_api->add_setting(g_mod, &s);

    // A row for each part: Underglow: Front, ...
    static char part_labels[kZoneCount][128];
    static const int part_order[kZoneCount] = {kZoneFront, kZoneRear, kZoneLeft, kZoneRight};
    for (int k = 0; k < kZoneCount; ++k) {
        const int z = part_order[k];
        snprintf(part_labels[z], sizeof part_labels[z], "%s: %s", tr("Underglow"), tr(kZoneLabels[z]));
        FH2Setting ps = s;
        ps.key = kPartSettings[z];
        ps.label = part_labels[z];
        ps.description = tr("Turns this part of the underglow on or off.");
        ps.value = c.part[z] ? 1 : 0;
        g_api->add_setting(g_mod, &ps);
    }

    for (int i = 0; i < kColorTotal; ++i) {
        const char* n = color_label(c, i, g_color_choice_text[i], sizeof g_color_choice_text[i]);
        if (n != g_color_choice_text[i]) strcpy_s(g_color_choice_text[i], n);
        g_color_choices[i] = g_color_choice_text[i];
    }
    FH2Setting col{};
    col.size = sizeof col;
    col.key = "NeonColor";
    col.label = tr("Primary Color");
    col.description = tr("The underglow's main color. Custom 1 to 10 are set in fh2-mods\\Underglow.ini ([CustomColors]).");
    col.value = c.color;
    col.on_change = on_setting;
    col.kind = FH2_SETTING_CHOICE;
    col.choices = g_color_choices;
    col.choice_count = kColorTotal;
    g_api->add_setting(g_mod, &col);

    FH2Setting col2 = col;
    col2.key = "NeonColor2";
    col2.label = tr("Secondary Color");
    col2.description = tr("The second color, for the effects with 2 colors and some audio visualizers.");
    col2.value = c.color2;
    g_api->add_setting(g_mod, &col2);

}

void anna_toggle(void*, const char*) { set_neon(!config().neon); }
const char* anna_toggle_label(void*, const char*) { return tr(config().neon ? "Underglow Off" : "Underglow On"); }

// A list's label, in the game's language: its user is the English.
const char* anna_text_label(void* user, const char*) { return tr(static_cast<const char*>(user)); }

// A color entry's user: which * 100 + color (which 1: the secondary).
// A part entry's user: its zone. Choosing it switches it; switching one on turns the underglow on too.
void anna_part(void* user, const char*) {
    const int z = (int)(intptr_t)user;
    const Config c = config();
    const bool on = !(c.part[z] && c.neon);
    set_part(z, on);
    if (on && !c.neon) set_neon(true);
}

const char* anna_part_label(void* user, const char*) {
    static char buf[128];
    const int z = (int)(intptr_t)user;
    const Config c = config();
    // What choosing it does, as ANNA's own switches say it ("Hazard Lights Off"): "Front Off" while it's lit.
    static const char* const on[kZoneCount] = {"Front On", "Right Side On", "Rear On", "Left Side On"};
    static const char* const off[kZoneCount] = {"Front Off", "Right Side Off", "Rear Off", "Left Side Off"};
    snprintf(buf, sizeof buf, "%s", tr(c.part[z] && c.neon ? off[z] : on[z]));
    return buf;
}

void anna_color(void* user, const char*) {
    const int u = (int)(intptr_t)user;
    set_color(u % 100, u / 100);
    if (!config().neon) set_neon(true);
}

// An effect entry's user: zone * 100 + effect, zone kZoneCount for all four.
void anna_effect(void* user, const char*) {
    const int u = (int)(intptr_t)user;
    set_effect(u / 100, u % 100);
    if (!config().neon) set_neon(true);
}

const char* anna_effect_label(void* user, const char*) {
    static char buf[128];
    const int u = (int)(intptr_t)user, zone = u / 100, i = u % 100;
    Config c = config();
    bool on = c.neon;
    for (int z = 0; z < kZoneCount; ++z)
        if (zone >= kZoneCount || z == zone) on = on && c.effect[z] == i;
    snprintf(buf, sizeof buf, on ? "» %s" : "%s", tr(kEffects[i]));   // the one in use: "» Comet" (no ▶ in the game's fonts)
    return buf;
}

const char* anna_color_label(void* user, const char*) {
    static char buf[128];   // ANNA copies the label straight away
    const int u = (int)(intptr_t)user, which = u / 100, i = u % 100;
    Config c = config();
    char name[64];
    const bool on = (which ? c.color2 : c.color) == i && c.neon;
    snprintf(buf, sizeof buf, on ? "» %s" : "%s", color_label(c, i, name, sizeof name));   // the one in use: "» Cyan"
    return buf;
}

// Car > Underglow is greyed out while the underglow is off (Car > Lights > Underglow On). ANNA greys a list out when
// all it shows is: so while it's off its lists hide and it shows only a stand-in that's always greyed out. (Not
// `enabled` on every entry: ANNA gives its rows a style trigger for each label that can be greyed out, and ~120 of
// them made moving through ANNA drop frames.)
int anna_underglow_on(void*, const char*) { return config().neon ? 1 : 0; }
int anna_underglow_off(void*, const char*) { return config().neon ? 0 : 1; }
int anna_never(void*, const char*) { return 0; }
void anna_nothing(void*, const char*) {}

// ANNA greys an entry out with the text it had when the HUD loaded (`label`, not label_now): so they're added from
// fh2mod_init when ANNA is already up (it loads first, by name), and the labels that show greyed are translated.
std::atomic<bool> g_anna_added{false};

void add_anna_entries_to(const FH2AnnaApi* anna) {
    if (g_anna_added.exchange(true)) return;
    FH2AnnaEntry t = {sizeof t};
    t.key = "underglow.toggle";
    t.parent = "car.lights";
    t.label = "Underglow On";
    t.on_choose = anna_toggle;
    t.label_now = anna_toggle_label;
    t.stays_open = 1;
    anna->add_entry(&t);

    // Car > Underglow: the colors and the effects.
    FH2AnnaEntry ug = {sizeof ug};
    ug.key = "underglow.underglow";
    ug.parent = "car";
    ug.label = tr("Underglow");   // what shows greyed out
    ug.label_now = anna_text_label;
    ug.user = (void*)"Underglow";
    anna->add_entry(&ug);
    FH2AnnaEntry stand_in = {sizeof stand_in};   // see anna_underglow_on
    stand_in.key = "underglow.off";
    stand_in.parent = "underglow.underglow";
    stand_in.label = tr("Underglow Off");
    stand_in.on_choose = anna_nothing;
    stand_in.shown = anna_underglow_off;
    stand_in.enabled = anna_never;
    anna->add_entry(&stand_in);

    static const char* const color_lists[2] = {"underglow.color", "underglow.color2"};
    static const char* const color_labels[2] = {"Primary Color", "Secondary Color"};
    static char keys[2][kColorTotal][32];
    for (int w = 0; w < 2; ++w) {
        FH2AnnaEntry list = {sizeof list};
        list.key = color_lists[w];
        list.parent = "underglow.underglow";
        list.label = color_labels[w];
        list.label_now = anna_text_label;
        list.user = (void*)color_labels[w];
        list.shown = anna_underglow_on;
        anna->add_entry(&list);
        for (int i = 0; i < kColorTotal; ++i) {
            snprintf(keys[w][i], sizeof keys[w][i], "%s.%d", color_lists[w], i);
            FH2AnnaEntry e = {sizeof e};
            e.key = keys[w][i];
            e.parent = color_lists[w];
            e.label = i < kColorCount ? kColors[i].name : "Custom";
            e.on_choose = anna_color;
            e.label_now = anna_color_label;
            e.user = (void*)(intptr_t)(w * 100 + i);
            e.stays_open = 1;
            anna->add_entry(&e);
        }
    }
    FH2AnnaEntry fx = {sizeof fx};
    fx.key = "underglow.effect";
    fx.parent = "underglow.underglow";
    fx.label = "Effect";
    fx.label_now = anna_text_label;
    fx.user = (void*)"Effect";
    fx.shown = anna_underglow_on;
    anna->add_entry(&fx);

    FH2AnnaEntry parts = {sizeof parts};
    parts.key = "underglow.parts";
    parts.parent = "underglow.underglow";
    parts.label = "Parts";
    parts.label_now = anna_text_label;
    parts.user = (void*)"Parts";
    parts.shown = anna_underglow_on;
    anna->add_entry(&parts);
    static const char* const part_keys[kZoneCount] = {"underglow.parts.front", "underglow.parts.right",
                                                      "underglow.parts.rear", "underglow.parts.left"};
    static const int part_menu[kZoneCount] = {kZoneFront, kZoneRear, kZoneLeft, kZoneRight};
    for (int k = 0; k < kZoneCount; ++k) {
        const int z = part_menu[k];
        FH2AnnaEntry e = {sizeof e};
        e.key = part_keys[z];
        e.parent = "underglow.parts";
        e.label = kZoneLabels[z];
        e.on_choose = anna_part;
        e.label_now = anna_part_label;
        e.user = (void*)(intptr_t)z;
        e.stays_open = 1;
        anna->add_entry(&e);
    }
    // Effect > All / Front / Rear / Left Side / Right Side > the groups > the effects.
    static const int zone_order[] = {kZoneCount, kZoneFront, kZoneRear, kZoneLeft, kZoneRight};
    static char zone_keys[kZoneCount + 1][32];
    static char group_keys[kZoneCount + 1][kGroupCount][40];
    static char fx_keys[kZoneCount + 1][kEffectCount][48];
    for (int zi = 0; zi < kZoneCount + 1; ++zi) {
        const int zone = zone_order[zi];
        snprintf(zone_keys[zi], sizeof zone_keys[zi], "underglow.effect.%s",
                 zone < kZoneCount ? kZoneNames[zone] : "All");
        FH2AnnaEntry zl = {sizeof zl};
        zl.key = zone_keys[zi];
        zl.parent = "underglow.effect";
        zl.label = zone < kZoneCount ? kZoneLabels[zone] : "All";
        zl.label_now = anna_text_label;
        zl.user = (void*)zl.label;
        anna->add_entry(&zl);
        for (int g = 0; g < kGroupCount; ++g) {
            snprintf(group_keys[zi][g], sizeof group_keys[zi][g], "%s.%s", zone_keys[zi], kGroupKeys[g]);
            FH2AnnaEntry ge = {sizeof ge};
            ge.key = group_keys[zi][g];
            ge.parent = zone_keys[zi];
            ge.label = kGroupNames[g];
            ge.label_now = anna_text_label;
            ge.user = (void*)kGroupNames[g];
            anna->add_entry(&ge);
        }
        for (int k = 0; k < kEffectCount; ++k) {
            const int i = kEffectOrder[k].effect;
            snprintf(fx_keys[zi][k], sizeof fx_keys[zi][k], "%s.%d", zone_keys[zi], i);
            FH2AnnaEntry e = {sizeof e};
            e.key = fx_keys[zi][k];
            e.parent = group_keys[zi][kEffectOrder[k].group];
            e.label = kEffects[i];
            e.on_choose = anna_effect;
            e.label_now = anna_effect_label;
            e.user = (void*)(intptr_t)(zone * 100 + i);
            e.stays_open = 1;
            anna->add_entry(&e);
        }
    }
    g_api->log(g_mod, "added to FH2 ANNA (Car > Lights and Car > Underglow)");
}

// Later, from the start thread, if ANNA wasn't up at fh2mod_init (greying out may then not show).
void add_anna_entries() {
    if (g_anna_added.load()) return;
    g_api->wait_for_mods(30000);
    const FH2AnnaApi* anna = static_cast<const FH2AnnaApi*>(g_api->get_interface(FH2ANNA_INTERFACE, 4));
    if (!anna) {
        g_api->log(g_mod, "FH2 ANNA (interface 4) isn't loaded: no ANNA entries; the Mods page still has the switch.");
        return;
    }
    add_anna_entries_to(anna);
}

// ---- Start ----------------------------------------------------------------------------------------------------

DWORD WINAPI start(LPVOID) {
    char* game = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!game) return 0;
    g_base = reinterpret_cast<uintptr_t>(game);

    if (memcmp(game + FH2_GAME_FORZACAR_KIND, FH2_GAME_FORZACAR_KIND_BYTES, sizeof FH2_GAME_FORZACAR_KIND_BYTES - 1) ||
        memcmp(game + FH2_GAME_FORZACAR_UPDATE, FH2_GAME_FORZACAR_UPDATE_BYTES,
               sizeof FH2_GAME_FORZACAR_UPDATE_BYTES - 1)) {
        g_api->log(g_mod, "this isn't the FH2 build Underglow knows (ForzaCar code differs): staying off");
        return 0;
    }

    struct {
        const char* name;
        uintptr_t at;
        void* detour;
        void** original;
    } const hooks[] = {
        {"ForzaCar::Update", FH2_GAME_FORZACAR_UPDATE, (void*)detour_car_update, (void**)&g_orig_car_update},
        {"LightManager beam preset upload", kBeamUpload, (void*)detour_beam_upload, (void**)&g_orig_beam},
        {"LightManager::CreateLight", kCreateLight, (void*)detour_create, (void**)&g_orig_create},
        {"LightManager::SetIntensity", kSetIntensity, (void*)detour_set_int, (void**)&g_orig_set_int},
    };
    for (const auto& h : hooks) {
        if (!g_api->hook_function(g_mod, h.name, game + h.at, h.detour, h.original)) {
            g_api->disable_hooks(g_mod, nullptr);
            g_api->log(g_mod, "couldn't hook the game (see above): no neon");
            return 0;
        }
    }
    // The neon's lights are placed with the game's own calls (not hooked).
    g_orig_set_pos = reinterpret_cast<SetVecFn>(game + kSetPosition);
    g_orig_set_dir = reinterpret_cast<SetVecFn>(game + kSetDirection);
    // The radio for the Audio Visualizer: not needed for the neon itself.
    if (!g_api->hook_function(g_mod, "FMOD System::update", game + kFmodUpdate, (void*)detour_fmod_update,
                              (void**)&g_orig_fmod_update))
        g_api->log(g_mod, "couldn't hook FMOD's update: the Audio Visualizer won't hear the radio");
    if (!g_api->enable_hooks(g_mod, nullptr)) {
        g_api->disable_hooks(g_mod, nullptr);
        g_api->log(g_mod, "couldn't switch the hooks on (see above): no neon");
        return 0;
    }
    g_api->log(g_mod, "hooked; the neon appears once the player's car is driving");
    add_anna_entries();
    return 0;
}

} // namespace

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (api->api_version < 9) return 1;   // commands, choice rows
    g_api = api;

    FH2ModInfo info{};
    info.size = sizeof info;
    info.name = "Underglow";
    info.version = UNDERGLOW_VERSION;
    info.github_repo = "qiangqiang101/FH2Underglow";
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.description = "Neon underglow made of the game's own lights, so it lights up the road: 61 effects (15 follow "
                       "the car radio's music), 28 colors, and each part of the car on its own. In ANNA: Car > Underglow.";
    info.check_updates = 1;
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;

    swprintf_s(g_mods_dir, MAX_PATH, L"%s\\fh2-mods", api->azimuth_dir);
    swprintf_s(g_ini, MAX_PATH, L"%s\\Underglow.ini", g_mods_dir);

    // Before 1.0 this mod was FH2Neon (FH2Neon.dll, FH2Neon.ini). Both loaded would be two underglows: with the old
    // one here, it's moved aside for the next start and this one stays off until then.
    wchar_t old_dll[MAX_PATH], old_ini[MAX_PATH];
    swprintf_s(old_dll, MAX_PATH, L"%s\\FH2Neon.dll", g_mods_dir);
    swprintf_s(old_ini, MAX_PATH, L"%s\\FH2Neon.ini", g_mods_dir);
    if (GetModuleHandleW(L"FH2Neon.dll")) {
        wchar_t aside[MAX_PATH];
        swprintf_s(aside, MAX_PATH, L"%s.old", old_dll);
        const bool moved = MoveFileExW(old_dll, aside, MOVEFILE_REPLACE_EXISTING) != 0;
        api->log(g_mod, moved ? "FH2Neon (this mod before 1.0) is loaded too: it's renamed FH2Neon.dll.old, and "
                                "Underglow starts from the next launch, with FH2Neon.ini's settings"
                              : "FH2Neon (this mod before 1.0) is loaded too: delete fh2-mods\\FH2Neon.dll; Underglow "
                                "stays off until it's gone");
        return 0;
    }
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(old_ini) != INVALID_FILE_ATTRIBUTES &&
        CopyFileW(old_ini, g_ini, TRUE))
        api->log(g_mod, "settings carried over from FH2Neon.ini to Underglow.ini");
    // Sections from 0.1 that nothing reads any more (editing them would do nothing).
    WritePrivateProfileStringW(L"Layout", nullptr, nullptr, g_ini);
    WritePrivateProfileStringW(L"NeonOverrides", nullptr, nullptr, g_ini);
    WritePrivateProfileStringW(L"Neon", L"Spread", nullptr, g_ini);
    WritePrivateProfileStringW(L"Neon", L"Count", nullptr, g_ini);
    WritePrivateProfileStringW(L"Neon", L"Lead", nullptr, g_ini);   // a test build's
    load_config();
    save_config();
    lang_load();   // the Mods page's rows are written in the language seen last
    add_settings();
    add_commands();
    if (const void* anna = api->get_interface(FH2ANNA_INTERFACE, 4))
        add_anna_entries_to(static_cast<const FH2AnnaApi*>(anna));

    if (HANDLE t = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, watch_ini, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}
