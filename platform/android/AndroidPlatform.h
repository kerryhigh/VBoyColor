#pragma once

#include "io/Platform.h"

#include <chrono>
#include <jni.h>
#include <set>

struct AAssetManager;

// Platform implementation for Android - JNI bridge to MainActivity.java's
// Storage Access Framework ROMs folder (SAF instead of a plain /sdcard path:
// at target SDK 34 that needs MANAGE_EXTERNAL_STORAGE, which kills and
// restarts the process the moment it's granted) plus misc device queries
// (battery).
class AndroidPlatform : public Platform
{
public:
    // vm/activityClazz/assetManager all come from android_app::activity -
    // called once at startup, before anything else here.
    AndroidPlatform(JavaVM *vm, jobject activityClazz, AAssetManager *assetManager);

    // Cardboard's Java classes must be resolved from the Activity class loader.
    // This queues SDK initialization on the UI thread before phone-mode
    // settings or rendering can use any Cardboard APIs.
    bool InitializeCardboardSdk();
    bool CardboardSdkInitializationComplete() const;

    bool HasRomsFolder() const override;
    bool SupportsChangeRomsFolder() const override { return true; }
    void RequestChangeRomsFolder() override;

    std::vector<RomEntry> ScanRoms() override;
    std::vector<uint8_t> ReadRomFile(const std::string &path) override;

    bool WriteRomsFile(const std::string &fileName, bool inStatesDir, const void *data, size_t size) override;
    std::vector<uint8_t> ReadRomsFile(const std::string &fileName, bool inStatesDir) override;
    bool RomsFileExists(const std::string &fileName, bool inStatesDir) const override;

    // Settings > Folders, with the system's folder picker (MainActivity's
    // pickFolder: right away on a phone, at the next start on a headset).
    bool SupportsDataFolders() const override { return true; }
    bool SupportsCardboardViewer() const override;
    void RequestCardboardViewerScan() override;
    void ShowCardboardMessage(const std::string &message) override;
    void SetPhoneMode(bool enabled) { m_phoneMode = enabled; }
    std::string DataFolderLabel(DataKind kind) const override;
    std::string SetDataFolder(DataKind kind, const std::string &path) override; // ("" only: back to the default)
    std::string PickDataFolder(DataKind kind) override;
    std::string TakeDataFolderStatus() override;

    int GetBatteryPercent() const override;

    // MainActivity's download thread (startDownload/pollDownload/takeDownload).
    bool StartDownload(const std::string &url, int slot = kDownloadBoxArt) override;
    int PollDownload(std::vector<uint8_t> &bytes, int slot = kDownloadBoxArt) override;
    // MainActivity's web requests (startHttp/pollHttp/takeHttp).
    int StartHttpRequest(const HttpRequest &request) override;
    int PollHttpRequest(int id, int &status, std::vector<uint8_t> &body) override;

    // Updates: the APK to the system's installer (MainActivity.installUpdate).
    const char *UpdatePackage() const override { return "android"; }
    std::string InstallUpdate(const std::vector<uint8_t> &package, bool &installed) override;
    std::string TakeUpdateStatus() override;
    void OpenUrl(const std::string &url) override;
    // Quit: the phone or TV app only (SetCanQuit - a headset's own menu
    // closes apps). MainActivity.finish(), then the main loop ends.
    void SetCanQuit(bool can) { m_canQuit = can; }
    bool CanQuit() const override { return m_canQuit; }
    void RequestQuit() override;
    bool QuitRequested() const override { return m_quit; }

    // The clipboard: Android's ClipboardManager (MainActivity.getClipboardText
    // / setClipboardText) - UTF-8 both ways as bytes, as JNI's own strings
    // are "modified" UTF-8.
    bool HasClipboard() const override { return true; }
    std::string GetClipboardText() override;
    void SetClipboardText(const std::string &text, bool sensitive) override;

    std::vector<uint8_t> LoadAssetBytes(const std::string &name) override;

private:
    // This thread's JNI environment - attached to the VM first if it isn't
    // yet (the library checks its games on threads of their own; they're
    // detached again when they end).
    JNIEnv *Env() const;
    // Calls a MainActivity method taking (int kind) and returning a String.
    std::string CallKindString(const char *method, DataKind kind) const;
    // Shared by ReadRomFile/ReadRomsFile - fd < 0 returns empty.
    static std::vector<uint8_t> ReadAllFromFd(int fd);

    JavaVM *m_vm = nullptr;
    jobject m_activity = nullptr; // global ref, set once by the constructor
    std::set<int> m_httpIds;      // web requests still to be reported
    int m_nextHttpId = 0;
    AAssetManager *m_assetManager = nullptr;
    bool m_canQuit = false, m_quit = false;
    bool m_phoneMode = false;
    std::chrono::steady_clock::time_point m_cardboardInitDeadline{};
};
