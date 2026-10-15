#pragma once

#include "input/Hotkeys.h"

#include "emu/ColorSchemes.h"
#include "input/ButtonMapping.h"

#include <openxr/openxr.h> // XrColor4f

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class Platform;

// kShadePalettes' entries made from the gradient patterns (kScreenPatterns):
// palette kFirstGradientShadePalette + n is pattern n (see the table below).
inline constexpr int kFirstGradientShadePalette = 6;

// AppSettings::selectedShadePalette's value for the Auto color mode: colors
// by what drew each pixel - its layer's depth, a sprite's palette (see
// core/emu/AutoColors.h) - under a game's color pack, if it has one. The
// default, so a game shows in color (its pack's, if any) out of the box.
inline constexpr int kAutoColors = ColorSchemes::kAutoColors;

// How the screen/menu quads react to head rotation - see
// OpenXrApp::ComputeScreenOrientation.
enum class FollowHeadMode : int32_t
{
    Off = 0,     // world-fixed, no head influence
    Smooth = 1,  // gradually chases the head orientation
    Instant = 2, // rigidly locked to the current head orientation every frame
};

// Persisted app-wide settings, one instance for the app's lifetime, threaded
// to every menu page via UiMenuResources::settings.
// How the game screen is drawn (Settings > Screen > Look - see
// shaders/screen_filter.frag): crisp pixels, smoothed edges, or the
// Virtual Boy's own LED rows with a glow.
enum class ScreenLook
{
    Sharp = 0,
    Smooth = 1,
    Led = 2,
};
inline constexpr int kScreenLookCount = 3;

// How a flat screen shows the Virtual Boy's two pictures (Settings > Screen >
// 3D - the PC window and the phone; a headset always shows them in 3D):
// one eye's, both side by side or one above the other for 3D TVs and
// monitors (half: squeezed, as the TV stretches each back to full size),
// or one picture for colored glasses (anaglyph - see shaders/anaglyph.frag).
enum class Screen3D
{
    Off = 0,
    SideBySideHalf = 1, // 3D TVs and monitors (Side-by-Side mode)
    SideBySideFull = 2, // each eye at its own shape (phone viewers, parallel viewing)
    CrossEyed = 3,      // full side by side, the right eye's picture on the left
    TopBottom = 4,      // 3D TVs' Top-and-Bottom mode
    RedCyan = 5,
    GreenMagenta = 6,
    AmberBlue = 7,
    TopBottomFull = 8,  // one above the other, each eye at its own shape
    GoogleCardboard = 9, // Android phone: SDK-calibrated lens distortion
};
inline constexpr int kScreen3DCount = 10;
// The anaglyph glasses (1 red/cyan, 2 green/magenta, 3 amber/blue - see
// shaders/anaglyph.frag), 0 for the other modes.
constexpr int AnaglyphOf(int screen3D) { return screen3D >= 5 && screen3D <= 7 ? screen3D - 4 : 0; }

// The menu shows 3D as two rows: the kind (off, side by side, top and
// bottom, colored glasses) and the kind's format (or glasses).
enum class Screen3DKind
{
    Off = 0,
    SideBySide = 1,
    TopBottom = 2,
    Glasses = 3,
};
inline constexpr int kScreen3DKindCount = 4;
constexpr Screen3DKind KindOf(int screen3D)
{
    switch (static_cast<Screen3D>(screen3D))
    {
    case Screen3D::SideBySideHalf:
    case Screen3D::SideBySideFull:
    case Screen3D::CrossEyed:
        return Screen3DKind::SideBySide;
    case Screen3D::TopBottom:
    case Screen3D::TopBottomFull:
        return Screen3DKind::TopBottom;
    case Screen3D::RedCyan:
    case Screen3D::GreenMagenta:
    case Screen3D::AmberBlue:
        return Screen3DKind::Glasses;
    default:
        return Screen3DKind::Off;
    }
}

struct AppSettings
{
    // Bumped when the on-disk settings schema changes. Known older versions
    // with append-only layouts are migrated in Settings.cpp.
    static constexpr int kVersion = 28;

    // Move Screen / Follow Head. Only OpenXrApp's quad-layer pose consumes
    // these - meaningless on the flat pc2d debug build.
    FollowHeadMode followHeadMode = FollowHeadMode::Off;
    float screenYaw = 0.0f;
    float screenPitch = 0.0f;
    float screenRoll = 0.0f;
    // meters - also doubles as the curved-screen cylinder radius (see
    // OpenXrApp::RenderScreenLayer). Menu distance is separate, see
    // kMenuDistanceMeters.
    float screenDistance = 2.2f;
    float screenScale = 1.0f;
    // Unused: the curved screen option was dropped (kept so the file's
    // layout stays the same).
    bool curvedScreen = false;

    bool useThreeDeeMode = true;
    float ipdOffset = 0.0f;  // meters
    int selectedPalette = 0; // authentic Virtual Boy red
    float colorR = 1.0f, colorG = 0.0f, colorB = 0.0f;
    // -1 = off (flat colorR/G/B tint above); 0-5 = index into kScreenPatterns,
    // a multi-hue gradient recolor instead. Mutually exclusive with the tint:
    // SettingsPage hides the R/G/B rows while this is >= 0.
    int selectedPattern = -1;

    // VB gameplay button remapping, indexed by VBButtonBit constants (see
    // Emulator.h); slots 1 and 9 are unused gaps and stay unbound. Each
    // button carries two independent physical bindings
    // (MappedButtons::Buttons[0]/[1]) - either triggers it.
    ButtonMapper::MappedButtons vbButtons[16];

    // -1 = off; 0+ = index into kShadePalettes - per-shade colors baked into
    // the frame itself by Emulator::SetShadePalette (Red Viper-style
    // colorization); kAutoColors = the Auto mode (the default). Mutually
    // exclusive with both the tint and selectedPattern above: while one's
    // active, ScreenTint()/ScreenPattern() go neutral and SettingsPage hides
    // the R/G/B rows. Kept as the LAST field so a version-11 settings file is
    // an exact prefix of this layout (see Settings.cpp's migration).
    int selectedShadePalette = kAutoColors;

    // Version 13: the room around the screen (passthrough, on headsets that
    // have it) instead of black. Appended - older files load as a prefix.
    bool passthrough = false;

    // Version 14 (appended - older files load as a prefix): the library as
    // a list instead of cards, and its thumbnails from libretro's box art
    // (downloaded) instead of title screens - on by default since version 16
    // (a game without box art gets its title screen; see Settings.cpp).
    bool libraryListView = false;
    bool downloadBoxArt = true;

    // Version 15 (appended): the library's last played games first.
    bool librarySortRecent = false;

    // Version 16 (appended): how the game screen is drawn - ScreenLook - and,
    // in the PC window, how big: as big as fits (false) or the biggest whole
    // multiple of its pixels (true). LED by default since version 18 (the
    // layout didn't change; an older file still on Sharp, the old default,
    // gets LED once - see Settings.cpp).
    int screenLook = static_cast<int>(ScreenLook::Led);
    bool screenWholePixels = false;

    // Version 17 (appended): the library shows only the games with a color
    // pack.
    bool libraryOnlyPacks = false;

    // Version 19 (appended): Screen3D - the flat apps' 3D (off: the left
    // eye's picture).
    int screen3D = static_cast<int>(Screen3D::Off);

    // Version 20 (appended): look for a new VBoy Color when the app starts
    // (About - see core/io/Updater.h).
    bool checkForUpdates = true;

    // Version 21 (appended): the window and phone apps have put the
    // gamepad's A/B by position (ButtonMapper::MigrateGamepadAToPosition) -
    // set once that has run.
    bool gamepadByPosition = false;

    // Version 22 (appended): the window app's hotkeys (Settings > Hotkeys -
    // see input/Hotkeys.h), indexed by HotkeyAction - their defaults filled
    // in once (hotkeysSet) - and whether F4 to F12 are the color pack and
    // video tools' (captures, re-import, recording... - see
    // platform/pc2d/Main.cpp) instead of hotkeys'.
    bool paintingTools = false;
    bool hotkeysSet = false;
    Hotkey hotkeys[kHotkeySlots];

    // Version 23 (appended): Yuno's Adventure's best score (the Support
    // page's little game - menu/pages/SupportPage.h).
    int32_t yunoBest = 0;

    // Version 24 (appended): Settings > Log window (Windows - see
    // Platform::ShowLogWindow).
    bool logWindow = false;

    // Version 25 (appended): the phone's on-screen controls (Settings >
    // Controls > Touch controls) - off, it's a gamepad (and Back for the menu).
    bool touchControls = true;

    // Version 26 (appended): a development builds' performance overlay, since
    // removed - kept so the layout (and a version 26 file) stays as it was.
    bool unused26 = false;

    // Version 27 (appended): magnification of the fixed Virtual Boy image in
    // Cardboard mode. The SDK still supplies all viewer optics; this only
    // scales the source image inside each calibrated eye view. A smaller image
    // occupies less of the calibrated view and feels farther away.
    static constexpr float kDefaultCardboardScale = 0.80f;
    static constexpr float kMinCardboardScale = 0.50f;
    static constexpr float kMaxCardboardScale = 1.25f;
    float cardboardScale = kDefaultCardboardScale;

    // What the screen should be drawn with (Emulator::DrawScreen's tint/
    // patternIndex) - neutral while a shade palette is active, since its
    // colors are already in the frame and tinting on top would double them.
    //
    // The per-shade palette the emulator colors with (Emulator::
    // SetShadePalette): the Multicolor one or kAutoColors, the modes that
    // show a game's color pack. Tint and Gradient never do - they only tint
    // the shades (a Multicolor palette made from a gradient colors with the
    // pack, and takes the gradient wherever the pack doesn't).
    int EffectiveShadePalette() const { return selectedShadePalette >= 0 ? selectedShadePalette : -1; }
    XrColor4f ScreenTint() const
    {
        return EffectiveShadePalette() >= 0 ? XrColor4f{1.0f, 1.0f, 1.0f, 1.0f} : XrColor4f{colorR, colorG, colorB, 1.0f};
    }
    int ScreenPattern() const { return EffectiveShadePalette() >= 0 ? -1 : selectedPattern; }

    // Best-effort; failures are silently ignored. Takes Platform by
    // reference rather than storing one - this struct is memcpy'd whole
    // to/from disk (see Settings.cpp), so it must stay trivially copyable.
    void Save(Platform &platform) const;

    // Replaces every field; returns false (self left untouched) if the file
    // doesn't exist or its version doesn't match kVersion. The file is
    // Platform::SettingsFileName (the PC VR app's own starts from the shared
    // settings.dat, without its button mapping).
    bool Load(Platform &platform);

private:
    bool LoadFile(Platform &platform, const char *fileName);
    bool LoadBytes(const std::vector<uint8_t> &bytes);

public:

    // Colors per game: the color settings (Color Mode, its palette, the
    // tint) are remembered for each game - SaveGameColors whenever they
    // change while it's loaded ("<game>.colors" in the States folder) - and
    // ApplyGameColors puts them back when it loads. A game with none saved
    // yet gets its suggested ones (a few games look best in a particular
    // palette - see Settings.cpp), else Auto. The tint's R/G/B carry over
    // from game to game unless a game has its own.
    void SaveGameColors(Platform &platform, const std::string &game) const;
    // romCrc (the ROM's CRC-32, 0 if unknown) finds a game's suggestion
    // whatever its file is called.
    void ApplyGameColors(Platform &platform, const std::string &game, uint32_t romCrc = 0);
};

// Preset VB screen colors. Index 0 is the authentic red Virtual Boy display
// and the factory default.
inline constexpr std::array<XrColor4f, ColorSchemes::kTintCount> kPredefColors = []()
{
    std::array<XrColor4f, ColorSchemes::kTintCount> out{};
    for (int i = 0; i < ColorSchemes::kTintCount; ++i)
        out[i] = {ColorSchemes::kTints[i].r, ColorSchemes::kTints[i].g, ColorSchemes::kTints[i].b, 1.0f};
    return out;
}();
inline constexpr int kPredefColorCount = static_cast<int>(kPredefColors.size());

// Named multi-hue gradients (5 stops each, darkest to brightest) - shared
// by screen_pattern.frag's rendering and the Color Palette row's preview
// swatches (SettingsPage::DrawColorPreview), so no color data is duplicated
// into the shader itself.
inline constexpr std::array<std::array<XrColor4f, 5>, ColorSchemes::kGradientCount> kScreenPatterns = []()
{
    std::array<std::array<XrColor4f, 5>, ColorSchemes::kGradientCount> out{};
    for (int p = 0; p < ColorSchemes::kGradientCount; ++p)
        for (int i = 0; i < 5; ++i)
        {
            const ColorSchemes::Rgb &c = ColorSchemes::kGradients[p][i];
            out[p][i] = {c.r, c.g, c.b, 1.0f};
        }
    return out;
}();
inline constexpr int kScreenPatternCount = static_cast<int>(kScreenPatterns.size());

// Per-shade ("Multicolor") palettes (AppSettings::selectedShadePalette) -
// one color for each of the VB's 4 shades: background (the VB's black), then
// the dark, light and lightest drawn shades (BRTA, BRTB, BRTA+BRTB+BRTC).
// Unlike the tint/patterns above, which recolor the finished frame by
// brightness, these color each pixel by which shade it is
// (core/emu/ShadeColorizer.h) - the colorization approach Red Viper
// (github.com/skyfloogle/red-viper) introduced, which keeps each layer's hue
// stable through fades. The colors are ColorSchemes::kMulticolor's; the
// index is what's saved to settings.dat.
inline constexpr std::array<std::array<XrColor4f, 4>, ColorSchemes::kMulticolorCount> kShadePalettes = []()
{
    std::array<std::array<XrColor4f, 4>, ColorSchemes::kMulticolorCount> out{};
    for (int p = 0; p < ColorSchemes::kMulticolorCount; ++p)
        for (int i = 0; i < 4; ++i)
        {
            const ColorSchemes::Rgb &c = ColorSchemes::kMulticolor[p].s[i];
            out[p][i] = {c.r, c.g, c.b, 1.0f};
        }
    return out;
}();
inline constexpr int kShadePaletteCount = static_cast<int>(kShadePalettes.size());

// The gradient pattern a per-shade palette was made from, or -1.
constexpr int GradientOfShadePalette(int palette)
{
    return palette >= kFirstGradientShadePalette && palette < kFirstGradientShadePalette + kScreenPatternCount
               ? palette - kFirstGradientShadePalette
               : -1;
}
static_assert(kShadePalettes[kFirstGradientShadePalette][1].g == kScreenPatterns[0][2].g &&
                  kShadePalettes[kFirstGradientShadePalette + kScreenPatternCount - 1][3].r == kScreenPatterns[kScreenPatternCount - 1][4].r,
              "kFirstGradientShadePalette must point at the FromPattern entries");
