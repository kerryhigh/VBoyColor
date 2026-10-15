#pragma once

#include "io/DataFolders.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct RomEntry
{
    std::string name;     // file name without extension, for display (e.g. "Golf (U) [!]")
    std::string fullPath; // for loading the ROM later - a filesystem path, or a content:// URI on Android
};

// Everything core/ needs from the OS/platform it's running on. One concrete
// implementation per platform (DesktopPlatform, AndroidPlatform), owned by
// that platform's entry point and threaded down through Initialize() calls -
// core/ itself never branches on a platform macro.
class Platform
{
public:
    virtual ~Platform() = default;

    // True once a still-accessible ROMs folder is picked.
    virtual bool HasRomsFolder() const = 0;
    // Whether this platform has a "Change ROMs Folder..." concept at all
    // (Android's SAF picker; desktop ROMs are just a fixed relative folder).
    virtual bool SupportsChangeRomsFolder() const = 0;
    // No-op if !SupportsChangeRomsFolder().
    virtual void RequestChangeRomsFolder() = 0;

    // Scans for .vb ROMs, sorted case-insensitively. Empty if there's
    // nowhere to look or no ROMs found.
    virtual std::vector<RomEntry> ScanRoms() = 0;
    // Reads a ROM's raw bytes given its RomEntry::fullPath - empty on error.
    virtual std::vector<uint8_t> ReadRomFile(const std::string &path) = 0;

    // Writable data (settings.dat, save states, .srm) - fileName resolved
    // relative to the ROMs folder. inStatesDir selects the folder root vs
    // the "States" subfolder (save states/previews).
    virtual bool WriteRomsFile(const std::string &fileName, bool inStatesDir, const void *data, size_t size) = 0;
    // Read counterpart to WriteRomsFile - empty if not found or unreadable.
    virtual std::vector<uint8_t> ReadRomsFile(const std::string &fileName, bool inStatesDir) = 0;
    virtual bool RomsFileExists(const std::string &fileName, bool inStatesDir) const = 0;
    // File names (not paths) directly inside a subfolder of the ROMs folder,
    // sorted. Optional: platforms that can't list folders (Android's SAF
    // grant) return none, which only disables importing color-pack
    // paintings there - finished packs (<rom>.vbcp) still load everywhere.
    virtual std::vector<std::string> ListRomsSubfolder(const std::string & /*subfolder*/) const { return {}; }

    // The settings file in the States folder. The PC VR app keeps its own
    // (settings-vr.dat): it shares the folder with the flat app, and the two
    // can't share a button mapping - the first binding of each button is a
    // key in one and a Touch controller input in the other.
    virtual const char *SettingsFileName() const { return "settings.dat"; }

    // ---- Settings > Folders (see io/DataFolders.h): a folder of the
    // player's choice for each kind of file. Read/WriteRomsFile go wherever
    // a file's kind lives. ----

    // Whether this platform lets the player choose them.
    virtual bool SupportsDataFolders() const { return false; }
    // Google Cardboard is a phone-only Android display path. Other platforms
    // keep the setting and QR scanner out of their menus.
    virtual bool SupportsCardboardViewer() const { return false; }
    virtual void RequestCardboardViewerScan() {}
    virtual void ShowCardboardMessage(const std::string & /*message*/) {}
    // What the menu shows for a kind's folder: its path or name, or "" for
    // the default place (inside the games folder).
    virtual std::string DataFolderLabel(DataKind /*kind*/) const { return ""; }
    // True: folders are picked in the app's own folder browser (the desktop
    // apps - a mouse, a gamepad or a headset) with the calls below; false:
    // with the system's picker (PickDataFolder - Android).
    virtual bool BrowsesDataFolders() const { return false; }
    // (Browser) the kind's folder now, as an absolute path (UTF-8).
    virtual std::string DataFolderPath(DataKind /*kind*/) const { return ""; }
    // (Browser) where browsing can start: drives, home, the app's folder -
    // pairs of (label, path).
    virtual std::vector<std::pair<std::string, std::string>> FolderPlaces() const { return {}; }
    // (Browser) the folders directly inside path, sorted (names only).
    virtual std::vector<std::string> ListFolders(const std::string & /*path*/) const { return {}; }
    // (Browser) points a kind at path ("" = back to its default place),
    // first copying over that kind's files the new folder doesn't have, or
    // has older copies of (kept as "<name>.bak"); the originals stay where
    // they were. Returns what happened, for the
    // menu ("" if nothing changed); on failure the folder stays as it was.
    virtual std::string SetDataFolder(DataKind /*kind*/, const std::string & /*path*/) { return ""; }
    // (System picker) asks the player for a kind's folder - right away, or
    // where that isn't possible (a headset: the picker would take VR focus)
    // the next time the app starts. Returns a line for the menu.
    virtual std::string PickDataFolder(DataKind /*kind*/) { return ""; }
    // (System picker) how the last change went, once it's done (the copying
    // runs in the background) - "" until then, and after it's been taken.
    virtual std::string TakeDataFolderStatus() { return ""; }

    // 0-100 device battery level, -1 if unavailable.
    virtual int GetBatteryPercent() const = 0;

    // Fetches an https URL in the background - one download at a time per
    // slot: kDownloadBoxArt (the library's optional box art) and
    // kDownloadUpdates (the updater's). False if this platform can't, or the
    // slot's download is still running.
    static constexpr int kDownloadBoxArt = 0, kDownloadUpdates = 1;
    virtual bool StartDownload(const std::string & /*url*/, int /*slot*/ = kDownloadBoxArt) { return false; }
    // The slot's download started last: 0 still running, 1 done (bytes
    // filled - reported once), -1 failed or none.
    virtual int PollDownload(std::vector<uint8_t> & /*bytes*/, int /*slot*/ = kDownloadBoxArt) { return -1; }

    // ---- Web requests (RetroAchievements - see achievements/Achievements.h):
    // any number at once, each in the background. ----
    struct HttpRequest
    {
        std::string url;
        std::string postData;    // sent as a POST when not empty (else a GET)
        std::string contentType; // postData's
        std::string userAgent;
    };
    // Starts one; returns its id (> 0), or 0 where this platform can't.
    virtual int StartHttpRequest(const HttpRequest & /*request*/) { return 0; }
    // 0 still running; 1 done - status is the HTTP status code (-1: no
    // connection), body what came back (reported once); -1 unknown id.
    virtual int PollHttpRequest(int /*id*/, int & /*status*/, std::vector<uint8_t> & /*body*/) { return -1; }

    // ---- Updates (core/io/Updater.h) ----

    // Which release download is this app's: "windows" (the .zip), "linux"
    // (the .tar.gz), "android" (the .apk) - "" where it can't update itself.
    virtual const char *UpdatePackage() const { return ""; }
    // Puts a downloaded release package in place. Desktop: replaces the
    // app's files (the running exe moved aside) and asks for a restart
    // (RestartRequested); Android: hands the APK to the system's installer
    // (on a headset when the app next starts). Returns a line for the menu;
    // installed: it's in place (or with the installer).
    virtual std::string InstallUpdate(const std::vector<uint8_t> & /*package*/, bool &installed)
    {
        installed = false;
        return "This version can't update itself";
    }
    // How an install that finishes later went (Android's installer), once -
    // "" until then.
    virtual std::string TakeUpdateStatus() { return ""; }
    // The app should close and start again (an update is in place) - the
    // main loop ends, cleans up as usual, then calls Relaunch.
    virtual bool RestartRequested() const { return false; }
    virtual void RequestRestart() {}
    // Opens a web page in the system's browser.
    virtual void OpenUrl(const std::string & /*url*/) {}

    // ---- Quit (the menu's sidebar, a hotkey): where the app can close
    // itself - the PC apps and a phone or TV (not a headset: its own menu
    // closes apps). The main loop ends once QuitRequested, and cleans up as
    // on any close. ----
    virtual bool CanQuit() const { return false; }
    // ---- The log window (Windows: the app runs without a console; Settings >
    // Log window opens one, for the curious and for bug reports). ----
    virtual bool HasLogWindow() const { return false; }
    virtual void ShowLogWindow(bool /*show*/) {}
    virtual void RequestQuit() {}
    virtual bool QuitRequested() const { return false; }

    // ---- The system clipboard (text): the text fields' Copy and Paste
    // (menu/pages/TextInputPage.h). ----
    virtual bool HasClipboard() const { return false; }
    // What's on it (UTF-8) - "" when it's empty or holds no text.
    virtual std::string GetClipboardText() { return ""; }
    // sensitive: a password - kept out of the clipboard's previews and
    // history where the system has a way to say so (Android 13+).
    virtual void SetClipboardText(const std::string & /*text*/, bool /*sensitive*/) {}

    // Loads a bundled app asset (font/icon/image) - APK assets on Android,
    // a file next to the running executable on desktop. Empty if not found.
    virtual std::vector<uint8_t> LoadAssetBytes(const std::string &name) = 0;
};
