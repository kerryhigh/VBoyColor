#include "android/AndroidPlatform.h"

#include <android/asset_manager.h>
#include <cardboard.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <android/log.h>

namespace
{
    constexpr const char *kLogTag = "VBoyColor";
    enum class CardboardSdkState { NotStarted, Pending, Ready, Failed };
    std::atomic<CardboardSdkState> g_cardboardSdkState{CardboardSdkState::NotStarted};

    void ClearJavaException(JNIEnv *env, const char *operation)
    {
        if (!env->ExceptionCheck())
            return;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s raised a Java exception", operation);
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    jboolean InitializeCardboardOnJavaThread(JNIEnv *env, jobject activity)
    {
        JavaVM *vm = nullptr;
        if (env->GetJavaVM(&vm) != JNI_OK || !vm)
        {
            g_cardboardSdkState.store(CardboardSdkState::Failed);
            return JNI_FALSE;
        }
        Cardboard_initializeAndroid(vm, activity);
        if (env->ExceptionCheck())
        {
            ClearJavaException(env, "Cardboard_initializeAndroid");
            g_cardboardSdkState.store(CardboardSdkState::Failed);
            return JNI_FALSE;
        }
        g_cardboardSdkState.store(CardboardSdkState::Ready);
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "Cardboard SDK initialized on the Activity thread");
        return JNI_TRUE;
    }

    // Detaches a thread Env() attached, when the thread ends.
    struct ThreadAttachment
    {
        JavaVM *vm = nullptr;
        ~ThreadAttachment()
        {
            if (vm)
                vm->DetachCurrentThread();
        }
    };
    thread_local ThreadAttachment t_attachment;
} // namespace

JNIEnv *AndroidPlatform::Env() const
{
    JNIEnv *env = nullptr;
    if (m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED &&
        m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK)
        t_attachment.vm = m_vm;
    return env;
}

AndroidPlatform::AndroidPlatform(JavaVM *vm, jobject activityClazz, AAssetManager *assetManager)
    : m_vm(vm), m_assetManager(assetManager)
{
    JNIEnv *env = nullptr;
    vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    m_activity = env->NewGlobalRef(activityClazz);
}

bool AndroidPlatform::InitializeCardboardSdk()
{
    g_cardboardSdkState.store(CardboardSdkState::Pending);
    m_cardboardInitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    if (!activityClass || env->ExceptionCheck())
    {
        ClearJavaException(env, "GetObjectClass(MainActivity)");
        g_cardboardSdkState.store(CardboardSdkState::Failed);
        return false;
    }

    JNINativeMethod nativeMethod{
        const_cast<char *>("initializeCardboardSdkNative"),
        const_cast<char *>("()Z"),
        reinterpret_cast<void *>(InitializeCardboardOnJavaThread)};
    if (env->RegisterNatives(activityClass, &nativeMethod, 1) != JNI_OK || env->ExceptionCheck())
    {
        ClearJavaException(env, "RegisterNatives(Cardboard initializer)");
        env->DeleteLocalRef(activityClass);
        g_cardboardSdkState.store(CardboardSdkState::Failed);
        return false;
    }

    jmethodID request = env->GetMethodID(activityClass, "requestCardboardSdkInitialization", "()Z");
    if (!request || env->ExceptionCheck())
    {
        ClearJavaException(env, "GetMethodID(requestCardboardSdkInitialization)");
        env->DeleteLocalRef(activityClass);
        g_cardboardSdkState.store(CardboardSdkState::Failed);
        return false;
    }

    const jboolean scheduled = env->CallBooleanMethod(m_activity, request);
    const bool failed = env->ExceptionCheck();
    if (failed)
        ClearJavaException(env, "requestCardboardSdkInitialization");
    env->DeleteLocalRef(activityClass);
    if (failed || scheduled != JNI_TRUE)
    {
        g_cardboardSdkState.store(CardboardSdkState::Failed);
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Couldn't queue Cardboard initialization; keeping phone mode available");
        return false;
    }
    return true;
}

bool AndroidPlatform::CardboardSdkInitializationComplete() const
{
    CardboardSdkState state = g_cardboardSdkState.load();
    if (state == CardboardSdkState::Pending && std::chrono::steady_clock::now() >= m_cardboardInitDeadline)
    {
        CardboardSdkState pending = CardboardSdkState::Pending;
        if (g_cardboardSdkState.compare_exchange_strong(pending, CardboardSdkState::Failed))
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Timed out waiting for the Cardboard UI-thread initializer");
        state = g_cardboardSdkState.load();
    }
    return state != CardboardSdkState::Pending;
}

bool AndroidPlatform::SupportsCardboardViewer() const
{
    return m_phoneMode && g_cardboardSdkState.load() == CardboardSdkState::Ready;
}

bool AndroidPlatform::HasRomsFolder() const
{
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID getUri = env->GetMethodID(activityClass, "getRomsTreeUriString", "()Ljava/lang/String;");
    auto uri = static_cast<jstring>(env->CallObjectMethod(m_activity, getUri));
    const bool has = uri != nullptr;
    env->DeleteLocalRef(activityClass);
    if (uri)
        env->DeleteLocalRef(uri);
    return has;
}

void AndroidPlatform::RequestChangeRomsFolder()
{
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID request = env->GetMethodID(activityClass, "requestChangeRomsFolder", "()V");
    env->CallVoidMethod(m_activity, request);
    env->DeleteLocalRef(activityClass);
}

void AndroidPlatform::RequestCardboardViewerScan()
{
    if (!SupportsCardboardViewer())
        return;
    CardboardQrCode_scanQrCodeAndSaveDeviceParams();
}

void AndroidPlatform::ShowCardboardMessage(const std::string &message)
{
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID show = env->GetMethodID(activityClass, "showCardboardMessage", "(Ljava/lang/String;)V");
    jstring text = env->NewStringUTF(message.c_str());
    env->CallVoidMethod(m_activity, show, text);
    env->DeleteLocalRef(text);
    env->DeleteLocalRef(activityClass);
}

std::vector<RomEntry> AndroidPlatform::ScanRoms()
{
    std::vector<RomEntry> result;
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID listMethod = env->GetMethodID(activityClass, "listRomFiles", "()[Ljava/lang/String;");
    auto array = static_cast<jobjectArray>(env->CallObjectMethod(m_activity, listMethod));
    env->DeleteLocalRef(activityClass);
    if (!array)
        return result;

    const jsize count = env->GetArrayLength(array);
    for (jsize i = 0; i + 1 < count; i += 2)
    {
        auto nameStr = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        auto uriStr = static_cast<jstring>(env->GetObjectArrayElement(array, i + 1));

        const char *nameChars = env->GetStringUTFChars(nameStr, nullptr);
        const char *uriChars = env->GetStringUTFChars(uriStr, nullptr);
        result.push_back({nameChars, uriChars});
        env->ReleaseStringUTFChars(nameStr, nameChars);
        env->ReleaseStringUTFChars(uriStr, uriChars);

        env->DeleteLocalRef(nameStr);
        env->DeleteLocalRef(uriStr);
    }
    env->DeleteLocalRef(array);

    // Already alphabetical from MainActivity.listRomFiles's own sort, but
    // ScanRoms() promises case-insensitive sorted order to every caller
    // regardless of platform - cheap enough not to special-case away.
    std::sort(result.begin(), result.end(), [](const RomEntry &a, const RomEntry &b)
              { return a.name < b.name; });
    return result;
}

std::vector<uint8_t> AndroidPlatform::ReadAllFromFd(int fd)
{
    std::vector<uint8_t> bytes;
    if (fd < 0)
        return bytes;

    FILE *f = fdopen(fd, "rb");
    if (!f)
    {
        close(fd);
        return bytes;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0)
    {
        bytes.resize(static_cast<size_t>(size));
        const size_t bytesRead = fread(bytes.data(), 1, bytes.size(), f);
        bytes.resize(bytesRead);
    }
    fclose(f); // also closes the underlying fd
    return bytes;
}

std::vector<uint8_t> AndroidPlatform::ReadRomFile(const std::string &path)
{
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID openMethod = env->GetMethodID(activityClass, "openRomFileDescriptor", "(Ljava/lang/String;)I");
    jstring uriJString = env->NewStringUTF(path.c_str());
    const jint fd = env->CallIntMethod(m_activity, openMethod, uriJString);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(uriJString);

    return ReadAllFromFd(fd);
}

bool AndroidPlatform::WriteRomsFile(const std::string &fileName, bool inStatesDir, const void *data, size_t size)
{
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID openMethod = env->GetMethodID(activityClass, "openDataFileForWrite", "(ILjava/lang/String;)I");
    const DataKind kind = DataKindOf(fileName, inStatesDir);
    jstring nameJString = env->NewStringUTF(NameInFolder(fileName, kind).c_str());
    const jint fd = env->CallIntMethod(m_activity, openMethod, static_cast<jint>(kind), nameJString);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(nameJString);

    if (fd < 0)
        return false;

    FILE *f = fdopen(fd, "wb");
    if (!f)
    {
        close(fd);
        return false;
    }
    const size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}

std::vector<uint8_t> AndroidPlatform::ReadRomsFile(const std::string &fileName, bool inStatesDir)
{
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID openMethod = env->GetMethodID(activityClass, "openDataFileForRead", "(ILjava/lang/String;)I");
    const DataKind kind = DataKindOf(fileName, inStatesDir);
    jstring nameJString = env->NewStringUTF(NameInFolder(fileName, kind).c_str());
    const jint fd = env->CallIntMethod(m_activity, openMethod, static_cast<jint>(kind), nameJString);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(nameJString);

    return ReadAllFromFd(fd);
}

bool AndroidPlatform::RomsFileExists(const std::string &fileName, bool inStatesDir) const
{
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID existsMethod = env->GetMethodID(activityClass, "dataFileExists", "(ILjava/lang/String;)Z");
    const DataKind kind = DataKindOf(fileName, inStatesDir);
    jstring nameJString = env->NewStringUTF(NameInFolder(fileName, kind).c_str());
    const bool exists = env->CallBooleanMethod(m_activity, existsMethod, static_cast<jint>(kind), nameJString);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(nameJString);
    return exists;
}

std::string AndroidPlatform::CallKindString(const char *method, DataKind kind) const
{
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID id = env->GetMethodID(activityClass, method, "(I)Ljava/lang/String;");
    env->DeleteLocalRef(activityClass);
    auto result = static_cast<jstring>(env->CallObjectMethod(m_activity, id, static_cast<jint>(kind)));
    std::string text;
    if (result)
    {
        const char *chars = env->GetStringUTFChars(result, nullptr);
        text = chars;
        env->ReleaseStringUTFChars(result, chars);
        env->DeleteLocalRef(result);
    }
    return text;
}

std::string AndroidPlatform::DataFolderLabel(DataKind kind) const
{
    return CallKindString("getFolderLabel", kind);
}

std::string AndroidPlatform::SetDataFolder(DataKind kind, const std::string &path)
{
    // (only back to the default place - other folders come from the picker)
    return path.empty() ? CallKindString("resetFolder", kind) : "";
}

void AndroidPlatform::RequestQuit()
{
    // (Activity.finish posts the close - the main loop sees destroyRequested)
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID finish = env->GetMethodID(activityClass, "finish", "()V");
    env->DeleteLocalRef(activityClass);
    if (finish)
        env->CallVoidMethod(m_activity, finish);
    m_quit = true;
}

std::string AndroidPlatform::PickDataFolder(DataKind kind)
{
    return CallKindString("pickFolder", kind);
}

std::string AndroidPlatform::TakeDataFolderStatus()
{
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID id = env->GetMethodID(activityClass, "takeFolderStatus", "()Ljava/lang/String;");
    env->DeleteLocalRef(activityClass);
    auto result = static_cast<jstring>(env->CallObjectMethod(m_activity, id));
    std::string text;
    if (result)
    {
        const char *chars = env->GetStringUTFChars(result, nullptr);
        text = chars;
        env->ReleaseStringUTFChars(result, chars);
        env->DeleteLocalRef(result);
    }
    return text;
}

int AndroidPlatform::GetBatteryPercent() const
{
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID getBatteryLevel = env->GetMethodID(activityClass, "getBatteryLevel", "()I");
    const int percent = env->CallIntMethod(m_activity, getBatteryLevel);
    env->DeleteLocalRef(activityClass);
    return percent;
}

bool AndroidPlatform::StartDownload(const std::string &url, int slot)
{
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID start = env->GetMethodID(activityClass, "startDownload", "(Ljava/lang/String;I)Z");
    jstring urlJString = env->NewStringUTF(url.c_str());
    const bool started = env->CallBooleanMethod(m_activity, start, urlJString, static_cast<jint>(slot));
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(urlJString);
    return started;
}

int AndroidPlatform::PollDownload(std::vector<uint8_t> &bytes, int slot)
{
    JNIEnv *env = Env();

    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID poll = env->GetMethodID(activityClass, "pollDownload", "(I)I");
    int state = env->CallIntMethod(m_activity, poll, static_cast<jint>(slot));
    if (state == 1)
    {
        jmethodID take = env->GetMethodID(activityClass, "takeDownload", "(I)[B");
        auto array = static_cast<jbyteArray>(env->CallObjectMethod(m_activity, take, static_cast<jint>(slot)));
        if (array)
        {
            const jsize length = env->GetArrayLength(array);
            bytes.resize(static_cast<size_t>(length));
            env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
            env->DeleteLocalRef(array);
        }
        else
            state = -1;
    }
    env->DeleteLocalRef(activityClass);
    return state;
}

int AndroidPlatform::StartHttpRequest(const HttpRequest &request)
{
    JNIEnv *env = Env();
    const int id = ++m_nextHttpId;
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID start = env->GetMethodID(activityClass, "startHttp",
                                       "(ILjava/lang/String;[BLjava/lang/String;Ljava/lang/String;)V");
    jstring url = env->NewStringUTF(request.url.c_str());
    jstring contentType = env->NewStringUTF(request.contentType.c_str());
    jstring userAgent = env->NewStringUTF(request.userAgent.c_str());
    jbyteArray post = nullptr;
    if (!request.postData.empty())
    {
        post = env->NewByteArray(static_cast<jsize>(request.postData.size()));
        env->SetByteArrayRegion(post, 0, static_cast<jsize>(request.postData.size()),
                                reinterpret_cast<const jbyte *>(request.postData.data()));
    }
    env->CallVoidMethod(m_activity, start, static_cast<jint>(id), url, post, contentType, userAgent);
    if (post)
        env->DeleteLocalRef(post);
    env->DeleteLocalRef(url);
    env->DeleteLocalRef(contentType);
    env->DeleteLocalRef(userAgent);
    env->DeleteLocalRef(activityClass);
    m_httpIds.insert(id);
    return id;
}

int AndroidPlatform::PollHttpRequest(int id, int &status, std::vector<uint8_t> &body)
{
    if (m_httpIds.count(id) == 0)
        return -1;
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID poll = env->GetMethodID(activityClass, "pollHttp", "(I)I");
    const jint result = env->CallIntMethod(m_activity, poll, static_cast<jint>(id));
    int state = 0;
    if (result != INT32_MIN)
    {
        jmethodID take = env->GetMethodID(activityClass, "takeHttp", "(I)[B");
        auto array = static_cast<jbyteArray>(env->CallObjectMethod(m_activity, take, static_cast<jint>(id)));
        body.clear();
        if (array)
        {
            const jsize length = env->GetArrayLength(array);
            body.resize(static_cast<size_t>(length));
            if (length > 0)
                env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte *>(body.data()));
            env->DeleteLocalRef(array);
        }
        status = static_cast<int>(result);
        m_httpIds.erase(id);
        state = 1;
    }
    env->DeleteLocalRef(activityClass);
    return state;
}

std::string AndroidPlatform::InstallUpdate(const std::vector<uint8_t> &package, bool &installed)
{
    installed = false;
    // (an APK is a zip - the cheapest sanity check)
    if (package.size() < 4 || package[0] != 'P' || package[1] != 'K')
        return "The download isn't an APK - nothing changed";
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID install = env->GetMethodID(activityClass, "installUpdate", "([B)Ljava/lang/String;");
    env->DeleteLocalRef(activityClass);
    jbyteArray array = env->NewByteArray(static_cast<jsize>(package.size()));
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(package.size()), reinterpret_cast<const jbyte *>(package.data()));
    auto result = static_cast<jstring>(env->CallObjectMethod(m_activity, install, array));
    env->DeleteLocalRef(array);
    std::string text;
    if (result)
    {
        const char *chars = env->GetStringUTFChars(result, nullptr);
        text = chars;
        env->ReleaseStringUTFChars(result, chars);
        env->DeleteLocalRef(result);
    }
    installed = text.compare(0, 8, "Couldn't") != 0;
    return text;
}

std::string AndroidPlatform::TakeUpdateStatus()
{
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID take = env->GetMethodID(activityClass, "takeInstallStatus", "()Ljava/lang/String;");
    env->DeleteLocalRef(activityClass);
    auto result = static_cast<jstring>(env->CallObjectMethod(m_activity, take));
    std::string text;
    if (result)
    {
        const char *chars = env->GetStringUTFChars(result, nullptr);
        text = chars;
        env->ReleaseStringUTFChars(result, chars);
        env->DeleteLocalRef(result);
    }
    return text;
}

void AndroidPlatform::OpenUrl(const std::string &url)
{
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID open = env->GetMethodID(activityClass, "openUrl", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(activityClass);
    jstring urlJString = env->NewStringUTF(url.c_str());
    env->CallVoidMethod(m_activity, open, urlJString);
    env->DeleteLocalRef(urlJString);
}

std::string AndroidPlatform::GetClipboardText()
{
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID get = env->GetMethodID(activityClass, "getClipboardText", "()[B");
    env->DeleteLocalRef(activityClass);
    auto array = static_cast<jbyteArray>(env->CallObjectMethod(m_activity, get));
    std::string text;
    if (array)
    {
        const jsize length = env->GetArrayLength(array);
        text.resize(static_cast<size_t>(length));
        if (length > 0)
            env->GetByteArrayRegion(array, 0, length, reinterpret_cast<jbyte *>(&text[0]));
        env->DeleteLocalRef(array);
    }
    return text;
}

void AndroidPlatform::SetClipboardText(const std::string &text, bool sensitive)
{
    JNIEnv *env = Env();
    jclass activityClass = env->GetObjectClass(m_activity);
    jmethodID set = env->GetMethodID(activityClass, "setClipboardText", "([BZ)V");
    env->DeleteLocalRef(activityClass);
    jbyteArray array = env->NewByteArray(static_cast<jsize>(text.size()));
    if (!text.empty())
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(text.size()), reinterpret_cast<const jbyte *>(text.data()));
    env->CallVoidMethod(m_activity, set, array, static_cast<jboolean>(sensitive));
    env->DeleteLocalRef(array);
}

std::vector<uint8_t> AndroidPlatform::LoadAssetBytes(const std::string &name)
{
    if (!m_assetManager)
        return {};
    AAsset *asset = AAssetManager_open(m_assetManager, name.c_str(), AASSET_MODE_BUFFER);
    if (!asset)
        return {};
    const off_t length = AAsset_getLength(asset);
    std::vector<uint8_t> data(static_cast<size_t>(length));
    AAsset_read(asset, data.data(), data.size());
    AAsset_close(asset);
    return data;
}
