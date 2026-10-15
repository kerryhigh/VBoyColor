#include "io/Settings.h"
#include "io/Platform.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr const char *kSettingsFileName = "settings.dat";

    // The versions below used append-only layouts. Copying an older prefix
    // keeps the player's existing settings and leaves newer fields at their
    // defaults. Version 27 included cardboardScale; version 28 changes its
    // untouched default without changing the stored layout.
    size_t PrefixCompatibleSize(int version)
    {
        switch (version)
        {
        case 11:
            return offsetof(AppSettings, selectedShadePalette);
        case 12:
            return offsetof(AppSettings, passthrough);
        case 13:
            return offsetof(AppSettings, libraryListView);
        case 14:
            return offsetof(AppSettings, librarySortRecent);
        case 15:
            return offsetof(AppSettings, screenLook);
        case 16:
            return offsetof(AppSettings, libraryOnlyPacks);
        case 17:
        case 18:
            return offsetof(AppSettings, screen3D);
        case 19:
            return offsetof(AppSettings, checkForUpdates);
        case 20:
            return offsetof(AppSettings, gamepadByPosition);
        case 21:
            return offsetof(AppSettings, paintingTools);
        case 22:
            return offsetof(AppSettings, yunoBest);
        case 23:
            return offsetof(AppSettings, logWindow);
        case 24:
            return offsetof(AppSettings, touchControls);
        case 25:
            return offsetof(AppSettings, unused26);
        case 26:
            return offsetof(AppSettings, cardboardScale);
        case 27:
            return offsetof(AppSettings, cardboardScale) + sizeof(float);
        default:
            return 0;
        }
    }

    // Shared by both platform Load paths.
    bool ApplyLoadedSettings(AppSettings &self, int version, const AppSettings &loaded)
    {
        if (version != AppSettings::kVersion)
            return false; // stale layout - leave self untouched, defaults stand

        self = loaded;
        return true;
    }
} // namespace

void AppSettings::Save(Platform &platform) const
{
    std::vector<uint8_t> bytes(sizeof(int) + sizeof(AppSettings));
    const int version = kVersion;
    std::memcpy(bytes.data(), &version, sizeof(version));
    std::memcpy(bytes.data() + sizeof(version), this, sizeof(AppSettings));
    platform.WriteRomsFile(platform.SettingsFileName(), true, bytes.data(), bytes.size());
}

bool AppSettings::Load(Platform &platform)
{
    std::vector<uint8_t> bytes = platform.ReadRomsFile(platform.SettingsFileName(), true);
    if (bytes.empty() && std::strcmp(platform.SettingsFileName(), kSettingsFileName) != 0)
    {
        // An app with a settings file of its own (the PC VR app), first run:
        // start from the shared file's settings (colors, screen look,
        // library), but not its button mapping - those are the other app's
        // keys (and, before the VR app had its own file, whatever it bound
        // over them). The app's defaults fill the mapping in.
        AppSettings shared;
        if (!shared.LoadFile(platform, kSettingsFileName))
            return false;
        for (ButtonMapper::MappedButtons &buttons : shared.vbButtons)
            buttons = {};
        *this = shared;
        return true;
    }
    return LoadBytes(bytes);
}

bool AppSettings::LoadFile(Platform &platform, const char *fileName)
{
    return LoadBytes(platform.ReadRomsFile(fileName, true));
}

bool AppSettings::LoadBytes(const std::vector<uint8_t> &bytes)
{
    if (bytes.size() < sizeof(int))
        return false;

    int version = 0;
    std::memcpy(&version, bytes.data(), sizeof(version));

    if (const size_t prefix = PrefixCompatibleSize(version); prefix && bytes.size() >= sizeof(int) + prefix)
    {
        AppSettings migrated; // fields past the prefix keep their defaults
        std::memcpy(&migrated, bytes.data() + sizeof(version), prefix);
        // Box art became the default in 16 - off was just the old default.
        if (version < 16)
            migrated.downloadBoxArt = true;
        // The LED look became the default in 18 - Sharp was just the old one.
        if (version < 18 && migrated.screenLook == static_cast<int>(ScreenLook::Sharp))
            migrated.screenLook = static_cast<int>(ScreenLook::Led);
        // Version 28 makes the default Cardboard image smaller for a more
        // comfortable viewing distance. Upgrade the old untouched default,
        // while keeping any scale the player had already adjusted.
        if (version == 27 && migrated.cardboardScale == 1.0f)
            migrated.cardboardScale = AppSettings::kDefaultCardboardScale;
        *this = migrated;
        return true;
    }

    if (bytes.size() < sizeof(int) + sizeof(AppSettings))
        return false;
    AppSettings loaded;
    std::memcpy(&loaded, bytes.data() + sizeof(version), sizeof(AppSettings));
    return ApplyLoadedSettings(*this, version, loaded);
}

namespace
{
    std::string GameColorsFile(const std::string &game) { return game + ".colors"; }

    using ColorSchemes::kSuggestions;
    using Suggestion = ColorSchemes::Suggestion;
} // namespace

void AppSettings::SaveGameColors(Platform &platform, const std::string &game) const
{
    if (game.empty())
        return;
    char text[160];
    const int n = std::snprintf(text, sizeof(text), "VBoyColor colors 1\nshade %d\npattern %d\npalette %d\ntint %.3f %.3f %.3f\n",
                                selectedShadePalette, selectedPattern, selectedPalette, colorR, colorG, colorB);
    if (n > 0)
        platform.WriteRomsFile(GameColorsFile(game), true, text, static_cast<size_t>(n));
}

void AppSettings::ApplyGameColors(Platform &platform, const std::string &game, uint32_t romCrc)
{
    if (game.empty())
        return;
    const std::vector<uint8_t> bytes = platform.ReadRomsFile(GameColorsFile(game), true);
    const std::string text(bytes.begin(), bytes.end());
    int shade = 0, pattern = 0, palette = 0;
    float r = 0, g = 0, b = 0;
    if (std::sscanf(text.c_str(), "VBoyColor colors 1 shade %d pattern %d palette %d tint %f %f %f", &shade, &pattern,
                    &palette, &r, &g, &b) == 6)
    {
        const bool shadeOk = shade == kAutoColors || (shade >= -1 && shade < kShadePaletteCount);
        if (shadeOk && pattern >= -1 && pattern < kScreenPatternCount && palette >= -1 && palette < kPredefColorCount)
        {
            selectedShadePalette = shade;
            selectedPattern = pattern;
            selectedPalette = palette;
            colorR = std::clamp(r, 0.0f, 1.0f);
            colorG = std::clamp(g, 0.0f, 1.0f);
            colorB = std::clamp(b, 0.0f, 1.0f);
            return;
        }
    }
    // Nothing saved for this game: its suggestion, else Auto.
    std::string lower = game;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    selectedShadePalette = kAutoColors;
    selectedPattern = -1;
    const Suggestion *found = nullptr;
    for (const Suggestion &suggestion : kSuggestions)
        if (romCrc && suggestion.crc == romCrc)
        {
            found = &suggestion;
            break;
        }
    for (const Suggestion &suggestion : kSuggestions)
        if (!found && lower.find(suggestion.nameContains) != std::string::npos)
            found = &suggestion;
    if (found)
    {
        selectedShadePalette = found->multicolor;
        selectedPattern = found->gradient;
    }
}
