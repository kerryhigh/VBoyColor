#include "menu/pages/SettingsPage.h"
#include "menu/pages/EmulatorButtonMapPage.h"
#include "menu/pages/FolderPage.h"
#include "io/Updater.h"
#include "emu/AutoColors.h"
#include "emu/Emulator.h"
#include "io/Platform.h"
#include "io/Settings.h"
#include "menu/MenuPage.h"
#include "menu/pages/AppMenuLayout.h"
#include "menu/ThumbnailLibrary.h"

#include <algorithm>
#include <cstdio>

namespace
{
std::string FormatFloat(const char *prefix, float value, int precision = 3, const char *suffix = "")
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%.*f%s", prefix, precision, value, suffix);
    return buf;
}

// Preview swatches drawn on the "Palette" row's right, showing the VB's 4
// brightness levels (black up to the full tint) or the palette's colors -
// stands in for the selected-palette index (nobody reads "7/11"; the colors
// themselves are what matters). Reads AppSettings live each frame, so it
// tracks palette/R/G/B edits immediately.
constexpr float kSwatchSize = 9.0f;
constexpr float kSwatchGap = 2.5f;

// The real hardware doesn't space its 4 brightness levels evenly (0, 1/3,
// 2/3, 1) - it's levels 0x00/0x63/0x87 out of a 0xff full intensity (level 3
// is whatever colorR/G/B is currently set to, since that already stands in
// for "full brightness" everywhere else in this app - the tint applied to
// the actual game screen).
constexpr float kBrightnessLevels[4] = {0.0f, 0x63 / 255.0f, 0x87 / 255.0f, 1.0f};

// The palette's colors, right-aligned in the row's value area.
void DrawSwatches(UiRenderer &ui, const XrColor4f *colors, int count, float rowX, float rowY, float rowW, float rowH, float alpha)
{
    float x = rowX + rowW - MenuList::kValueRightPad - count * (kSwatchSize + kSwatchGap) + kSwatchGap;
    const float y = rowY + (rowH - kSwatchSize) / 2.0f;
    for (int i = 0; i < count; ++i)
    {
        ui.DrawQuadRounded(x, y, kSwatchSize, kSwatchSize, XrColor4f{colors[i].r, colors[i].g, colors[i].b, alpha}, 2.5f);
        x += kSwatchSize + kSwatchGap;
    }
}

void DrawColorPreview(AppSettings *settings, UiRenderer &ui, float rowX, float rowY, float rowW, float rowH, float alpha)
{
    if (!settings)
        return;
    XrColor4f colors[5];
    int count = 0;
    // Auto: its layers' colors, far to near, and its sprites' red - what the
    // screen mostly shows (see AutoColors.h).
    if (settings->selectedShadePalette == kAutoColors)
    {
        const AutoColors::Rgb swatches[5] = {AutoColors::kLayerRamps[0][1], AutoColors::kLayerRamps[1][1],
                                             AutoColors::kLayerRamps[2][1], AutoColors::kLayerRamps[3][1],
                                             AutoColors::kSpriteRamps[0][1]};
        for (const AutoColors::Rgb &c : swatches)
            colors[count++] = XrColor4f{c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f};
    }
    // A per-shade palette made from a gradient picks its shades' colors from
    // all 5 stops, so those show; otherwise its own 4 colors.
    else if (settings->selectedShadePalette >= 0 && settings->selectedShadePalette < kShadePaletteCount &&
             GradientOfShadePalette(settings->selectedShadePalette) >= 0)
    {
        for (const XrColor4f &stop : kScreenPatterns[GradientOfShadePalette(settings->selectedShadePalette)])
            colors[count++] = stop;
    }
    else if (settings->selectedShadePalette >= 0 && settings->selectedShadePalette < kShadePaletteCount)
    {
        for (const XrColor4f &shade : kShadePalettes[settings->selectedShadePalette])
            colors[count++] = shade;
    }
    // A pattern's 5 gradient stops - the same kScreenPatterns the screen
    // shader reads (see Settings.h).
    else if (settings->selectedPattern >= 0 && settings->selectedPattern < kScreenPatternCount)
    {
        for (const XrColor4f &stop : kScreenPatterns[settings->selectedPattern])
            colors[count++] = stop;
    }
    else
    {
        for (int i = 0; i < 4; ++i)
        {
            const float level = kBrightnessLevels[i];
            colors[count++] = XrColor4f{settings->colorR * level, settings->colorG * level, settings->colorB * level, 1.0f};
        }
    }
    DrawSwatches(ui, colors, count, rowX, rowY, rowW, rowH, alpha);
}
} // namespace

void SettingsPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    m_settings = resources.settings;
    m_platform = resources.platform;
    m_emulator = resources.emulator;
    m_library = resources.thumbnails;

    auto list = MakeList(ui, resources);

    list->AddHeader("Controls");
    list->AddEntry("Button mapping", [this](MenuItem *)
                   {
                       if (!emulatorButtonMapPage)
                           return;
                       emulatorButtonMapPage->OpenFor("", "", nullptr); // (the general mapping)
                       emulatorButtonMapPage->sidebarItem = sidebarItem;
                       Navigate(emulatorButtonMapPage, 1);
                   }, nullptr, nullptr, UiIconId::Mapping)
        ->opensPage = true;
    // The phone: move the on-screen controls (see TouchControls' edit mode).
    if (resources.buttonMappingProfile == ButtonMappingProfile::Phone)
    {
        // (off: a gamepad plays, and Back opens the menu)
        auto shown = list->AddEntry("Touch controls", [this](MenuItem *)
                                    {
                                        m_settings->touchControls = !m_settings->touchControls;
                                        m_settings->Save(*m_platform);
                                    },
                                    nullptr, nullptr, UiIconId::Mapping);
        shown->toggle = [this]() { return m_settings && m_settings->touchControls; };
        const std::function<void(const std::string &)> *edit = &resources.editTouchControls;
        list->AddEntry("Arrange touch controls", [edit](MenuItem *)
                       { if (*edit) (*edit)(""); }, nullptr, nullptr, UiIconId::Move)
            ->SetValue("Edit");
    }
    // The window app's hotkeys (and the painting tools' keys).
    if (resources.buttonMappingProfile == ButtonMappingProfile::Desktop)
        list->AddEntry("Hotkeys", [this](MenuItem *)
                       { if (hotkeysPage) Navigate(hotkeysPage, 1); }, nullptr, nullptr, UiIconId::Mapping)
            ->opensPage = true;
    // (a flat window or phone has no screen to place)
    if (resources.buttonMappingProfile == ButtonMappingProfile::Vr)
        list->AddEntry("Adjust screen", [this](MenuItem *)
                       { if (moveScreenPage) Navigate(moveScreenPage, 1); }, nullptr, nullptr, UiIconId::Move)
            ->opensPage = true;

    // Color mode picks the kind of coloring (see ColorMode in
    // SettingsPage.h); Palette then only cycles that mode's presets, so each
    // list stays short and the modes are discoverable by name.
    list->AddHeader("Colors");
    m_colorModeEntry = list->AddEntry("Color mode", [this](MenuItem *) { ChangeColorMode(1); }, // Select acts like Right
        [this](MenuItem *) { ChangeColorMode(-1); }, [this](MenuItem *) { ChangeColorMode(1); }, UiIconId::Palette);
    m_paletteEntry = list->AddEntry("Palette", [this](MenuItem *) { ChangePalette(1); }, // Select acts like Right - advance the palette
        [this](MenuItem *) { ChangePalette(-1); }, [this](MenuItem *) { ChangePalette(1); }, UiIconId::None,
        [this](UiRenderer &ui, float x, float y, float w, float h, float a) { DrawColorPreview(m_settings, ui, x, y, w, h, a); });
    m_paletteEntry->reserveIconSpace = true; // indented under Color mode, like the R/G/B rows
    m_colorREntry = list->AddEntry("Red", [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorR, kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorR, -kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorR, kColorStep); });
    m_colorGEntry = list->AddEntry("Green", [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorG, kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorG, -kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorG, kColorStep); });
    m_colorBEntry = list->AddEntry("Blue", [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorB, kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorB, -kColorStep); },
        [this](MenuItem *) { ChangeColorChannel(&AppSettings::colorB, kColorStep); });
    m_colorREntry->reserveIconSpace = true;
    m_colorGEntry->reserveIconSpace = true;
    m_colorBEntry->reserveIconSpace = true;

    // How the game screen is drawn (see shaders/screen_filter.frag).
    list->AddHeader("Screen");
    m_lookEntry = list->AddEntry("Look", [this](MenuItem *) { ChangeLook(1); }, [this](MenuItem *) { ChangeLook(-1); },
                                 [this](MenuItem *) { ChangeLook(1); }, UiIconId::FlatScreen);
    if (resources.buttonMappingProfile != ButtonMappingProfile::Vr)
    {
        // The PC window or the phone: as big as fits, or whole multiples of the pixels.
        auto toggleSize = [this](MenuItem *)
        {
            m_settings->screenWholePixels = !m_settings->screenWholePixels;
            RefreshLabels();
        };
        m_sizeEntry = list->AddEntry("Size", toggleSize, toggleSize, toggleSize, UiIconId::Scale);
        if (m_platform->SupportsCardboardViewer())
            m_cardboardScaleEntry = list->AddEntry("Cardboard scale", [this](MenuItem *) { ChangeCardboardScale(0.05f); },
                                                   [this](MenuItem *) { ChangeCardboardScale(-0.05f); },
                                                   [this](MenuItem *) { ChangeCardboardScale(0.05f); }, UiIconId::Scale);
        // A flat screen's 3D (see Screen3D): 3D TVs and monitors, colored glasses.
        m_threeDEntry = list->AddEntry("3D", [this](MenuItem *) { Change3D(1); }, [this](MenuItem *) { Change3D(-1); },
                                       [this](MenuItem *) { Change3D(1); }, UiIconId::ThreeD);
        m_threeDFormatEntry = list->AddEntry("Format", [this](MenuItem *) { Change3DFormat(1); },
                                             [this](MenuItem *) { Change3DFormat(-1); },
                                             [this](MenuItem *) { Change3DFormat(1); });
        if (m_platform->SupportsCardboardViewer())
            m_cardboardScanEntry = list->AddEntry("Scan viewer QR code", [this](MenuItem *)
                                                  { m_platform->RequestCardboardViewerScan(); },
                                                  nullptr, nullptr, UiIconId::ThreeD);
    }

    list->AddHeader("Library");
    // Box art from libretro's thumbnail collection instead of title screens
    // (on by default; a game without one gets its title screen).
    auto boxArt = list->AddEntry("Download box art", [this](MenuItem *)
                                 {
                                     m_settings->downloadBoxArt = !m_settings->downloadBoxArt;
                                     m_settings->Save(*m_platform);
                                     if (m_library)
                                         m_library->SetBoxArt(m_settings->downloadBoxArt);
                                 },
                                 nullptr, nullptr, UiIconId::Save);
    boxArt->toggle = [this]() { return m_settings && m_settings->downloadBoxArt; };
    // (also the library's own filter chip)
    auto onlyPacks = list->AddEntry("Only games with color packs", [this](MenuItem *)
                                    {
                                        m_settings->libraryOnlyPacks = !m_settings->libraryOnlyPacks;
                                        m_settings->Save(*m_platform);
                                        if (m_library)
                                            m_library->SetOnlyPacks(m_settings->libraryOnlyPacks);
                                    },
                                    nullptr, nullptr, UiIconId::Palette);
    onlyPacks->toggle = [this]() { return m_settings && m_settings->libraryOnlyPacks; };
    m_rebuildEntry = list->AddEntry("Make thumbnails again", [this](MenuItem *)
                                    { if (m_library) m_library->RebuildAll(); }, nullptr, nullptr, UiIconId::Reset);
    // Where each kind of file goes (see io/DataFolders.h, FolderPage).
    if (m_platform->SupportsDataFolders())
    {
        list->AddHeader("Folders");
        for (int i = 0; i < kDataKindCount; ++i)
        {
            const DataKind kind = static_cast<DataKind>(i);
            m_folderEntries[i] = list->AddEntry(DataKindName(kind), [this, kind](MenuItem *)
                                                {
                                                    if (!folderPage)
                                                        return;
                                                    folderPage->Open(kind);
                                                    Navigate(folderPage, 1);
                                                },
                                                nullptr, nullptr, i == 0 ? UiIconId::RomList : i == 1 ? UiIconId::Save : i == 2 ? UiIconId::SaveSlot : i == 3 ? UiIconId::Settings : UiIconId::FlatScreen);
            m_folderEntries[i]->opensPage = true;
        }
    }
    else if (m_platform->SupportsChangeRomsFolder())
    {
        // Way back into the ROMs-folder picker (SAF, on Android). Clears the
        // folder and asks for a restart rather than re-popping the picker
        // directly (see Platform::RequestChangeRomsFolder).
        m_changeRomsFolderEntry = list->AddEntry("Change ROMs folder", [this](MenuItem *) { RequestChangeRomsFolder(); },
            nullptr, nullptr, UiIconId::RomList);
    }

    // Troubleshooting: development builds only (a push's build from GitHub
    // Actions, "dev.N") - releases don't show it. Windows: the app has no
    // console - the log, in a window of its own, when asked for.
    if (resources.updater && resources.updater->IsDevBuild() && m_platform->HasLogWindow())
    {
        list->AddHeader("Troubleshooting");
        {
            auto log = list->AddEntry("Log window", [this](MenuItem *)
                                      {
                                          m_settings->logWindow = !m_settings->logWindow;
                                          m_platform->ShowLogWindow(m_settings->logWindow);
                                          m_settings->Save(*m_platform);
                                      },
                                      nullptr, nullptr, UiIconId::Header);
            log->toggle = [this]() { return m_settings && m_settings->logWindow; };
        }
    }

    m_menu.MenuItems.push_back(list);
    m_menu.Init();

    RefreshLabels(false);
}

std::string SettingsPage::Subtitle() const
{
    if (!m_emulator || m_emulator->RomName().empty())
        return "";
    const std::string &name = m_emulator->RomName();
    return "Colors for " + name.substr(0, name.find(" ("));
}

void SettingsPage::Update(uint32_t *buttonState, uint32_t *lastButtonState, float deltaSeconds)
{
    MenuPage::Update(buttonState, lastButtonState, deltaSeconds);
    if (m_rebuildEntry && m_library)
    {
        const char *value = m_library->Remaining() > 0 ? "Making..." : "";
        if (m_rebuildEntry->value != value)
            m_rebuildEntry->SetValue(value);
    }
}

SettingsPage::ColorMode SettingsPage::CurrentColorMode() const
{
    // Derived from which palette field is set rather than stored, so it can
    // never disagree with what's actually being drawn.
    if (m_settings->selectedShadePalette == kAutoColors)
        return ColorMode::Auto;
    if (m_settings->selectedShadePalette >= 0)
        return ColorMode::Multicolor;
    if (m_settings->selectedPattern >= 0)
        return ColorMode::Gradient;
    return ColorMode::Tint;
}

void SettingsPage::ChangeColorMode(int delta)
{
    if (!m_settings)
        return;

    // Remember the palette being left, so cycling back to its mode returns
    // to it (this session only - the active one is what's saved to disk).
    const ColorMode current = CurrentColorMode();
    if (current == ColorMode::Gradient)
        m_lastPattern = m_settings->selectedPattern;
    else if (current == ColorMode::Multicolor)
        m_lastShadePalette = m_settings->selectedShadePalette;

    const int next = (static_cast<int>(current) + delta + kColorModeCount) % kColorModeCount;
    // Tint needs nothing set: colorR/G/B (and selectedPalette) are kept
    // untouched while another mode is active, so the previous tint returns.
    m_settings->selectedPattern = next == static_cast<int>(ColorMode::Gradient) ? m_lastPattern : -1;
    m_settings->selectedShadePalette = next == static_cast<int>(ColorMode::Multicolor) ? m_lastShadePalette
                                       : next == static_cast<int>(ColorMode::Auto)     ? kAutoColors
                                                                                        : -1;
    RefreshLabels();
}

void SettingsPage::ChangePalette(int delta)
{
    if (!m_settings)
        return;
    // Cycles only the presets of the current Color Mode - see ChangeColorMode.
    switch (CurrentColorMode())
    {
    case ColorMode::Tint:
    {
        // -1 = a custom R/G/B color - start cycling from the first preset.
        int index = m_settings->selectedPalette >= 0 ? m_settings->selectedPalette : 0;
        index = (index + delta + kPredefColorCount) % kPredefColorCount;
        m_settings->selectedPalette = index;
        m_settings->colorR = kPredefColors[index].r;
        m_settings->colorG = kPredefColors[index].g;
        m_settings->colorB = kPredefColors[index].b;
        break;
    }
    case ColorMode::Gradient:
        m_settings->selectedPattern = (m_settings->selectedPattern + delta + kScreenPatternCount) % kScreenPatternCount;
        break;
    case ColorMode::Multicolor:
        m_settings->selectedShadePalette =
            (m_settings->selectedShadePalette + delta + kShadePaletteCount) % kShadePaletteCount;
        break;
    case ColorMode::Auto:
        break; // no presets - the row just shows what Auto uses
    }
    RefreshLabels();
}

void SettingsPage::ChangeColorChannel(float AppSettings::*channel, float delta)
{
    if (!m_settings)
        return;
    float &value = m_settings->*channel;
    value += delta;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    m_settings->selectedPalette = -1; // diverges from whatever preset was selected, matches FrontendGo
    m_settings->selectedPattern = -1; // R/G/B rows are only reachable in tint mode anyway, but stay defensive
    m_settings->selectedShadePalette = -1;
    RefreshLabels();
}

void SettingsPage::ChangeLook(int delta)
{
    if (!m_settings)
        return;
    m_settings->screenLook = (m_settings->screenLook + delta + kScreenLookCount) % kScreenLookCount;
    RefreshLabels();
}

namespace
{
    // Each 3D kind's modes, in the order its second row steps through them.
    struct Screen3DFormats
    {
        const Screen3D *modes;
        const char *const *names;
        int count;
    };
    Screen3DFormats FormatsOf(Screen3DKind kind)
    {
        static constexpr Screen3D kSbs[] = {Screen3D::SideBySideHalf, Screen3D::SideBySideFull, Screen3D::CrossEyed};
        static constexpr const char *kSbsNames[] = {"3D TV (squeezed)", "Full", "Cross-eyed"};
        static constexpr Screen3D kTb[] = {Screen3D::TopBottom, Screen3D::TopBottomFull};
        static constexpr const char *kTbNames[] = {"3D TV (squeezed)", "Full"};
        static constexpr Screen3D kGlasses[] = {Screen3D::RedCyan, Screen3D::GreenMagenta, Screen3D::AmberBlue};
        static constexpr const char *kGlassesNames[] = {"Red / cyan", "Green / magenta", "Amber / blue"};
        switch (kind)
        {
        case Screen3DKind::SideBySide: return {kSbs, kSbsNames, 3};
        case Screen3DKind::TopBottom: return {kTb, kTbNames, 2};
        case Screen3DKind::Glasses: return {kGlasses, kGlassesNames, 3};
        default: return {nullptr, nullptr, 0};
        }
    }
} // namespace

void SettingsPage::Change3D(int delta)
{
    if (!m_settings)
        return;
    if (m_platform && m_platform->SupportsCardboardViewer())
    {
        // Keep every existing flat mode and its order, with Cardboard as a
        // fifth, distinct display mode on Android phones only.
        static constexpr Screen3D kPhoneModes[] = {Screen3D::Off, Screen3D::SideBySideHalf,
                                                   Screen3D::TopBottom, Screen3D::RedCyan,
                                                   Screen3D::GoogleCardboard};
        int at = 0;
        const Screen3D current = static_cast<Screen3D>(m_settings->screen3D);
        if (current == Screen3D::SideBySideHalf || current == Screen3D::SideBySideFull || current == Screen3D::CrossEyed)
            at = 1;
        else if (current == Screen3D::TopBottom || current == Screen3D::TopBottomFull)
            at = 2;
        else if (current == Screen3D::RedCyan || current == Screen3D::GreenMagenta || current == Screen3D::AmberBlue)
            at = 3;
        else if (current == Screen3D::GoogleCardboard)
            at = 4;
        at = (at + (delta > 0 ? 1 : -1) + 5) % 5;
        m_settings->screen3D = static_cast<int>(kPhoneModes[at]);
        RefreshLabels();
        return;
    }
    // The next kind, at its first format (3D TV / red-cyan).
    const int kind = (static_cast<int>(KindOf(m_settings->screen3D)) + delta + kScreen3DKindCount) % kScreen3DKindCount;
    const Screen3DFormats formats = FormatsOf(static_cast<Screen3DKind>(kind));
    m_settings->screen3D = static_cast<int>(formats.count ? formats.modes[0] : Screen3D::Off);
    RefreshLabels();
}

void SettingsPage::Change3DFormat(int delta)
{
    if (!m_settings)
        return;
    const Screen3DFormats formats = FormatsOf(KindOf(m_settings->screen3D));
    if (!formats.count)
        return;
    int at = 0;
    for (int i = 0; i < formats.count; ++i)
        if (static_cast<int>(formats.modes[i]) == m_settings->screen3D)
            at = i;
    m_settings->screen3D = static_cast<int>(formats.modes[(at + delta + formats.count) % formats.count]);
    RefreshLabels();
}

void SettingsPage::ChangeCardboardScale(float delta)
{
    if (!m_settings)
        return;
    m_settings->cardboardScale = std::clamp(m_settings->cardboardScale + delta, 0.75f, 1.25f);
    RefreshLabels();
}

void SettingsPage::RequestChangeRomsFolder()
{
    m_platform->RequestChangeRomsFolder();
    if (m_changeRomsFolderEntry)
        m_changeRomsFolderEntry->SetText("Folder cleared - restart the app!");
}

void SettingsPage::OnShow()
{
    RefreshLabels(false); // (a game's own colors may have loaded since)
}

void SettingsPage::RefreshLabels(bool save)
{
    if (!m_settings)
        return;

    // The Palette row draws the actual colors via DrawColorPreview instead
    // of a selected-index number (see AddEntry's accessoryDraw above).
    static constexpr const char *kModeNames[kColorModeCount] = {"Tint", "Gradient", "Multicolor", "Auto"};
    m_colorModeEntry->SetValue(kModeNames[static_cast<int>(CurrentColorMode())]);
    static constexpr const char *kLookNames[kScreenLookCount] = {"Sharp", "Smooth", "LED"};
    m_lookEntry->SetValue(kLookNames[std::clamp(m_settings->screenLook, 0, kScreenLookCount - 1)]);
    if (m_sizeEntry)
    {
        const bool cardboard = m_platform && m_platform->SupportsCardboardViewer() &&
                               m_settings->screen3D == static_cast<int>(Screen3D::GoogleCardboard);
        m_sizeEntry->SetText(cardboard ? "Pixel scaling" : "Size");
        m_sizeEntry->SetValue(m_settings->screenWholePixels ? "Whole pixels" : "Fit");
    }
    if (m_cardboardScaleEntry)
    {
        const bool cardboard = m_platform && m_platform->SupportsCardboardViewer() &&
                               m_settings->screen3D == static_cast<int>(Screen3D::GoogleCardboard);
        m_cardboardScaleEntry->Visible = cardboard;
        m_cardboardScaleEntry->SetValue(FormatFloat("", m_settings->cardboardScale * 100.0f, 0, "%"));
    }
    for (int i = 0; i < kDataKindCount; ++i)
        if (m_folderEntries[i])
        {
            const std::string label = m_platform->DataFolderLabel(static_cast<DataKind>(i));
            const size_t at = label.find_last_of("/\\", label.size() > 1 ? label.size() - 2 : 0);
            std::string shown = at == std::string::npos ? label : label.substr(at + 1);
            if (!shown.empty() && (shown.back() == '/' || shown.back() == '\\') && shown.size() > 1)
                shown.pop_back();
            m_folderEntries[i]->SetValue(shown.empty() ? (i == 0 ? "Not chosen" : "In the games folder") : shown);
        }
    if (m_threeDEntry)
    {
        static constexpr const char *kKinds[kScreen3DKindCount] = {"Off", "Side by side", "Top and bottom", "Colored glasses"};
        const Screen3DKind kind = KindOf(m_settings->screen3D);
        const bool cardboard = m_platform && m_platform->SupportsCardboardViewer() &&
                               m_settings->screen3D == static_cast<int>(Screen3D::GoogleCardboard);
        m_threeDEntry->SetValue(cardboard ? "Google Cardboard" : kKinds[static_cast<int>(kind)]);
        const Screen3DFormats formats = FormatsOf(kind);
        m_threeDFormatEntry->Visible = formats.count > 0 && !cardboard;
        if (formats.count)
        {
            m_threeDFormatEntry->SetText(kind == Screen3DKind::Glasses ? "Glasses" : "Format");
            for (int i = 0; i < formats.count; ++i)
                if (static_cast<int>(formats.modes[i]) == m_settings->screen3D)
                    m_threeDFormatEntry->SetValue(formats.names[i]);
        }
    }
    if (m_cardboardScanEntry)
        m_cardboardScanEntry->SetValue("Custom viewer profile");
    // 2 decimals, not 3 - kColorStep is 0.05, so the third decimal is always
    // 0 and never actually reachable by adjusting the value.
    m_colorREntry->SetValue(FormatFloat("", m_settings->colorR, 2));
    m_colorGEntry->SetValue(FormatFloat("", m_settings->colorG, 2));
    m_colorBEntry->SetValue(FormatFloat("", m_settings->colorB, 2));
    // Auto has no presets to step through.
    const bool autoMode = CurrentColorMode() == ColorMode::Auto;
    if (autoMode != !m_paletteEntry->leftFunction)
    {
        if (autoMode)
            m_paletteEntry->leftFunction = m_paletteEntry->rightFunction = nullptr;
        else
        {
            m_paletteEntry->leftFunction = [this](MenuItem *) { ChangePalette(-1); };
            m_paletteEntry->rightFunction = [this](MenuItem *) { ChangePalette(1); };
        }
    }
    // A gradient pattern or per-shade palette has no R/G/B of its own to
    // adjust - hide those rows entirely while one's selected (see
    // MenuList::Entry::Visible).
    const bool tintMode = CurrentColorMode() == ColorMode::Tint;
    m_colorREntry->Visible = tintMode;
    m_colorGEntry->Visible = tintMode;
    m_colorBEntry->Visible = tintMode;

    if (!save)
        return;
    m_settings->Save(*m_platform); // always-on autosave - no explicit save action anywhere in the menu anymore
    // ...and remembered for the game being played (see AppSettings::SaveGameColors),
    // whose thumbnail then shows them too.
    if (m_emulator)
    {
        m_settings->SaveGameColors(*m_platform, m_emulator->RomName());
        if (m_library)
            m_library->Recheck(m_emulator->RomName());
    }
}
