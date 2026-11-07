# Underglow

Neon underglow for **Forza Horizon 2** on PC (Azimuth), made of the game's own lights, so it really lights up the road under your car. A mod for the [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform), switched from [FH2 ANNA](https://github.com/qiangqiang101/FH2_ANNA).

## Installing

You need Azimuth 1.0.1 or later. Download `Underglow-v1.0.0.zip` from [Releases](https://github.com/qiangqiang101/FH2Underglow/releases) and extract it into the folder with `azimuth.exe`:

```
DWrite.dll                 the FH2 Mod Platform
fh2-mods\Underglow.dll     this mod
```

[FH2 ANNA](https://github.com/qiangqiang101/FH2_ANNA) has its menus (Car > Underglow); without it there's the Mods page and the commands.

**Coming from FH2Neon** (this mod before 1.0): your settings come over by themselves (`FH2Neon.ini` is copied to `Underglow.ini`). Delete `fh2-mods\FH2Neon.dll`. If it's still there, Underglow renames it `FH2Neon.dll.old` and starts from the next launch, so you never get two underglows.

## Using it

- **FH2 ANNA**, Car > Lights: **Underglow On / Off**.
- **FH2 ANNA**, Car > **Underglow**:
  - **Primary Color** and **Secondary Color**: 18 colors and your 10 custom ones.
  - **Effect** > All, Front, Rear, Left Side or Right Side > a group > the effect. Each part of the car can play its own effect.
  - **Parts**: switch the front, the rear and each side on or off (only the sides, say).
- **Pause > Settings > MODS**: Underglow (On/Off), each part On/Off, Primary Color and Secondary Color. There are more effects than a row there can list, so they're in ANNA.
- **Commands** (see [Commands](#commands)): `fh2mod Underglow effect comet`, `fh2mod Underglow effect left police`, `fh2mod Underglow part front off`.

The menus are in the game's language (Brazilian Portuguese, German, French, Spanish, Mexican Spanish, Italian, Japanese, Korean, Traditional Chinese, Polish, Russian and Dutch; Czech and Hungarian show English, as the game does there).

The underglow stops with the game: in the pause menu and in photo mode its effects hold still.

### Effects

The number is the effect's number in the ini.

- **Audio Visualizer**: Audio Visualizer (8), Audio Pulse (17), Peak Meter (18), Audio Blend (19), Bass Flash (20), Audio Sparkle (21), Center Burst (23), Audio Rainbow (22), VU Meter (29), Audio Sine (26), Audio Sync (28), Audio Party (25), Audio Star (27), Audio Bubbles (24), Audio Swirl (30)
- **Effects with 1 Color** (the primary): Static (0), Breathe (3), Smooth Blink (37), Blink (6), Wipe (4), Fill (14), Stack (5), Comet (13), Visor (16), Marquee (40), Motion Point (34), Bouncing Ball (31), Rain (35), Sparkle Fade (38), Rotating Beam (36), Breathing Circle (32), Zig Zag (39), Fractal Motion (33), Lightning (11)
- **Effects with 2 Colors** (the primary and the secondary): Crossing Beams (9), Swap (15), Alternate Blink (41), Gradient Wave (42), Sequence (45), Wavy (46), Two-Color Marquee (47), Moving Panes (43), Noise Map (44)
- **Multicolor Effects** (their own colors): Rainbow (1), Rainbow Wave (2), Color Wheel (50), Rotating Rainbow (56), Double Rainbow (51), Radial Rainbow (54), Spiral (57), Random Spin (55), Hypnotoad (10), Swirl Circles (60), Motion Points (52), Random Marquee (12), Mosaic (53), Starry Night (58), Bubbles (49), Bloom (48), Sunrise (59), Police (7)

Many of the effects from Audio Bubbles on are after those of [OpenRGB Effects Plugin](https://gitlab.com/OpenRGBDevelopers/OpenRGBEffectsPlugin), written afresh for a ring of lights round a car. The ones drawn as a picture (Color Wheel, Spiral, Rotating Beam, Bubbles, Breathing Circle...) use where each light is on the car.

- Rainbow Wave, Comet, the marquees and Gradient Wave run round the car through every part that plays the same effect (and skip the parts that are off).
- The rest play on each part by itself. The front and the rear run left to right, and both sides run front to back.
- Police is red on the car's left and blue on its right.
- The audio effects listen to the car radio, and only the radio: not the engine, the tyres, or the festival's stage music when your radio is off. The rear shows the bass, the left side the low mids, the right side the high mids, and the front the treble. Each band is levelled to the music, so the radio volume doesn't matter, and silence or a radio that's off stays dark. Only if Underglow can't hook the game's sound at all do they play a made-up beat. `fh2mod Underglow status` says which one it's using.

### The ini

Settings are kept in `fh2-mods\Underglow.ini`, and saving it while the game runs applies it within half a second:

```ini
[Settings]
Neon=1
Color=0              ; the primary color: its place in the menu (0 Cyan ... 17 Lavender, 18-27 Custom 1-10)
Color2=3             ; the secondary color, the same way
Brightness=1         ; 1 = as bright as your headlight
FrontEffect=0        ; an effect's number (see Effects)
RightEffect=0
RearEffect=0
LeftEffect=0
FrontOn=1            ; each part on (1) or off (0)
RightOn=1
RearOn=1
LeftOn=1
Speed=0.3            ; effect cycles a second

[Neon]
Side=10              ; lights down each side
Front=6              ; lights across the front
Rear=6               ; lights across the back
Height=0.2           ; metres above the ground
FrontInset=0.9       ; metres from the headlights back to the front wheels
RearInset=1          ; metres from the tail lights forward to the rear wheels
SideOffset=0.1       ; metres out from the car's side (negative: in)
SideTrim=0.45        ; metres the side strips stop short of each wheel, clear of the wheel arches
Range=3              ; how far each light reaches
Power=2
Inner=60             ; the light's cone, degrees
Outer=170

[CustomColors]
Color1=0 1 1         ; 0 to 1, or 0 to 255 ("255 105 180"), or hex ("#FF69B4")
Name1=               ; the name in the menus (blank: Custom 1)
; ... up to Color10 / Name10
```

Keep notes on lines of their own: a `;` after a value is read as part of it.

## Commands

Through the platform's command tool (`fh2mod.exe`, in the platform zip's `tools` folder), while the game runs:

```
fh2mod Underglow status                       # the neon, its colors, effects and parts, and the radio
fh2mod Underglow neon on|off
fh2mod Underglow color Red                    # the primary color: a name, Custom1..Custom10, r g b or #RRGGBB
fh2mod Underglow color2 #FF69B4               # the secondary color
fh2mod Underglow effect [front|right|rear|left|all] <name or number>
fh2mod Underglow part [front|right|rear|left|all] on|off
fh2mod Underglow speed 0.5
fh2mod Underglow brightness 0.8
fh2mod Underglow tune SideTrim 0.8            # any of the [Neon] values, saved to the ini
```

## How it works

FH2's ForwardPlus renderer keeps all the lights of a scene in one light system: a vector of 64-byte records, each pointing at a preset. Every frame the game uploads it to the GPU (`0x10F6C40` beams, `0x10F6970` glows). [Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) found the path from the player's car to its own headlight records (`fh2/features/headlight_beams.c`). Underglow creates its lights through the game's own light manager, each a copy of your headlight with a preset of its own (always on, its own color), placed round the car between the wheels and pointing down. The source ([src/neon_mod.cpp](src/neon_mod.cpp)) has the whole chain of offsets at the top.

The audio effects put an FMOD FFT on the game's radio bus, and read whether the car radio is on from the game's RadioSystem. The effects' clock is the player car's own update time, so they stop when the game does.

## Building

You need CMake and Visual Studio 2022. The FH2 Mod Platform and FH2 ANNA (for its header, `include/fh2anna.h`) are submodules:

```sh
git clone --recursive https://github.com/qiangqiang101/FH2Underglow.git
cd FH2Underglow
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release      # build/Release/Underglow.dll
```

Put `Underglow.dll` in `fh2-mods` next to `azimuth.exe` (with the platform's `DWrite.dll` next to `azimuth.exe`).

`tools/vfont.py` and `tools/check_text.py` check the translations against the game's fonts, which only have the characters the game's own text uses (a missing one shows as a box).
