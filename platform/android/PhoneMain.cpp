// The phone and tablet app: the same APK as the Quest's, run flat when the
// device isn't a headset (MainActivity.isHeadset - see AndroidMain.cpp's
// android_main). Like the PC window (platform/pc2d/Main.cpp): the game
// screen and the menu drawn with Vulkan into the app's window - here with
// the Virtual Boy controller on the touch screen (core/input/TouchControls)
// and any gamepad the phone has. The touch controls hide while a gamepad is
// in use and come back on the next touch. Android's Back opens the menu (in
// the menu it goes back); fingers tap and drag in the menu like the Quest's
// lasers.
#define VK_USE_PLATFORM_ANDROID_KHR
#include "android/PhoneMain.h"
#include "android/AndroidPlatform.h"
#include "emu/Emulator.h"
#include "gfx/UiRenderer.h"
#include "gfx/VulkanRenderer.h"
#include "input/ButtonMapping.h"
#include "input/TouchControls.h"
#include "io/Settings.h"
#include "menu/AppMenu.h"
#include "menu/VirtualBoyFrame.h"
#include "menu/pages/AppMenuLayout.h"
#include <cardboard.h>

#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <dlfcn.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define LOG_TAG "VBoyColor"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace
{
    // Android 11+: tells the system this window shows a 50 fps game (0: no
    // preference), so a phone with a 50, 100 or 60 Hz mode can use it instead
    // of redrawing the same game frame at 90 or 120 Hz - less GPU work, and
    // steadier pacing. (dlsym: Android 10 doesn't have it.)
    void HintFrameRate(ANativeWindow *window, float fps)
    {
        using SetFrameRate = int32_t (*)(ANativeWindow *, float, int8_t);
        static const SetFrameRate setFrameRate = []() -> SetFrameRate
        {
            void *lib = dlopen("libandroid.so", RTLD_NOW);
            return lib ? reinterpret_cast<SetFrameRate>(dlsym(lib, "ANativeWindow_setFrameRate")) : nullptr;
        }();
        if (!window || !setFrameRate)
            return;
        setFrameRate(window, fps, 1 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE */);
    }
} // namespace

namespace
{
    struct Finger
    {
        float x = 0, y = 0;
        bool up = false; // lifted - kept one more frame, so even the quickest tap is seen
    };

    struct PhoneState
    {
        bool resumed = false;
        ANativeWindow *window = nullptr;
        bool windowChanged = false;
        uint32_t gamepadKeyBits = 0, gamepadAxisBits = 0;
        bool gamepadGuideHeld = false;
        bool gamepadUsed = false; // since the last touch
        bool backHeld = false;
        // Back pressed since the last frame - a quick tap (the back gesture)
        // can be down and up again before the frame looks at backHeld.
        bool backTapped = false;
        // The touch controls shown (see the frame: Settings' switch, a gamepad, a touch).
        bool touchShown = true;
        double lastTouchSeconds = 0.0;
        std::map<int32_t, Finger> fingers;
        bool touched = false; // since the last frame
        // Lets go of the Vulkan surface: called while Android still waits
        // for the window to be released (APP_CMD_TERM_WINDOW).
        std::function<void()> releaseWindow;
    };

    void CheckVk(VkResult result, const char *what)
    {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string("Vulkan call failed: ") + what + " (" + std::to_string(result) + ")");
    }

    void HandleAppCmd(android_app *app, int32_t cmd)
    {
        auto *state = static_cast<PhoneState *>(app->userData);
        switch (cmd)
        {
        case APP_CMD_INIT_WINDOW:
            state->window = app->window;
            state->windowChanged = true;
            break;
        case APP_CMD_TERM_WINDOW:
            // (the swapchain goes now - the window is only valid until this returns)
            if (state->releaseWindow)
                state->releaseWindow();
            state->window = nullptr;
            state->windowChanged = true;
            break;
        case APP_CMD_RESUME:
            state->resumed = true;
            break;
        case APP_CMD_PAUSE:
            state->resumed = false;
            state->gamepadKeyBits = state->gamepadAxisBits = 0;
            state->gamepadGuideHeld = state->backHeld = false;
            state->fingers.clear();
            break;
        default:
            break;
        }
    }

    // AKEYCODE_* -> ButtonMapper::EmuButton_* bit (as AndroidMain.cpp's).
    uint32_t GamepadKeyBit(int32_t keyCode)
    {
        using namespace ButtonMapper;
        switch (keyCode)
        {
        case AKEYCODE_BUTTON_A:
            return ButtonMapping[EmuButton_A];
        case AKEYCODE_BUTTON_B:
            return ButtonMapping[EmuButton_B];
        case AKEYCODE_BUTTON_X:
            return ButtonMapping[EmuButton_X];
        case AKEYCODE_BUTTON_Y:
            return ButtonMapping[EmuButton_Y];
        case AKEYCODE_BUTTON_L1:
            return ButtonMapping[EmuButton_LShoulder];
        case AKEYCODE_BUTTON_R1:
            return ButtonMapping[EmuButton_RShoulder];
        case AKEYCODE_BUTTON_L2:
            return ButtonMapping[EmuButton_L2];
        case AKEYCODE_BUTTON_R2:
            return ButtonMapping[EmuButton_R2];
        case AKEYCODE_BUTTON_START:
            return ButtonMapping[EmuButton_Enter];
        case AKEYCODE_BUTTON_SELECT:
            return ButtonMapping[EmuButton_Back];
        case AKEYCODE_DPAD_UP:
            return ButtonMapping[EmuButton_Up];
        case AKEYCODE_DPAD_DOWN:
            return ButtonMapping[EmuButton_Down];
        case AKEYCODE_DPAD_LEFT:
            return ButtonMapping[EmuButton_Left];
        case AKEYCODE_DPAD_RIGHT:
            return ButtonMapping[EmuButton_Right];
        default:
            return 0;
        }
    }

    int32_t HandleInputEvent(android_app *app, AInputEvent *event)
    {
        auto *state = static_cast<PhoneState *>(app->userData);
        const int32_t source = AInputEvent_getSource(event);
        const bool gamepad = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
                             (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;

        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY)
        {
            const int32_t keyCode = AKeyEvent_getKeyCode(event);
            const bool down = AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN;
            if (keyCode == AKEYCODE_BACK && !gamepad)
            {
                state->backHeld = down; // (the menu - not leaving the app)
                if (down && AKeyEvent_getRepeatCount(event) == 0)
                    state->backTapped = true;
                return 1;
            }
            if (!gamepad)
                return 0;
            if (keyCode == AKEYCODE_BUTTON_MODE)
            {
                state->gamepadGuideHeld = down;
                state->gamepadUsed = true;
                return 1;
            }
            const uint32_t bit = GamepadKeyBit(keyCode);
            if (bit == 0)
                return keyCode == AKEYCODE_BACK ? 1 : 0; // (a pad's B can arrive as Back)
            if (down)
                state->gamepadKeyBits |= bit;
            else
                state->gamepadKeyBits &= ~bit;
            state->gamepadUsed = true;
            return 1;
        }

        if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
            return 0;

        if ((source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN)
        {
            const int32_t raw = AMotionEvent_getAction(event);
            const int32_t action = raw & AMOTION_EVENT_ACTION_MASK;
            const size_t index = static_cast<size_t>((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                                     AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            const size_t count = AMotionEvent_getPointerCount(event);
            for (size_t i = 0; i < count; ++i)
            {
                Finger &f = state->fingers[AMotionEvent_getPointerId(event, i)];
                f.x = AMotionEvent_getX(event, i);
                f.y = AMotionEvent_getY(event, i);
            }
            if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP)
                state->fingers[AMotionEvent_getPointerId(event, action == AMOTION_EVENT_ACTION_UP ? 0 : index)].up = true;
            if (action == AMOTION_EVENT_ACTION_CANCEL)
                for (auto &entry : state->fingers)
                    entry.second.up = true;
            if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN)
                state->fingers[AMotionEvent_getPointerId(event, index)].up = false;
            state->touched = true;
            state->gamepadUsed = false;
            return 1;
        }

        if (!gamepad)
            return 0;
        using namespace ButtonMapper;
        constexpr float kDeadzone = 0.5f;
        auto axis = [event](int32_t id) { return AMotionEvent_getAxisValue(event, id, 0); };
        const float lx = axis(AMOTION_EVENT_AXIS_X), ly = axis(AMOTION_EVENT_AXIS_Y);
        const float rx = axis(AMOTION_EVENT_AXIS_Z), ry = axis(AMOTION_EVENT_AXIS_RZ);
        const float hx = axis(AMOTION_EVENT_AXIS_HAT_X), hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
        uint32_t bits = 0;
        if (lx < -kDeadzone) bits |= ButtonMapping[EmuButton_LeftStickLeft];
        if (lx > kDeadzone) bits |= ButtonMapping[EmuButton_LeftStickRight];
        if (ly < -kDeadzone) bits |= ButtonMapping[EmuButton_LeftStickUp];
        if (ly > kDeadzone) bits |= ButtonMapping[EmuButton_LeftStickDown];
        if (rx < -kDeadzone) bits |= ButtonMapping[EmuButton_RightStickLeft];
        if (rx > kDeadzone) bits |= ButtonMapping[EmuButton_RightStickRight];
        if (ry < -kDeadzone) bits |= ButtonMapping[EmuButton_RightStickUp];
        if (ry > kDeadzone) bits |= ButtonMapping[EmuButton_RightStickDown];
        if (hx < -kDeadzone) bits |= ButtonMapping[EmuButton_Left];
        if (hx > kDeadzone) bits |= ButtonMapping[EmuButton_Right];
        if (hy < -kDeadzone) bits |= ButtonMapping[EmuButton_Up];
        if (hy > kDeadzone) bits |= ButtonMapping[EmuButton_Down];
        // (triggers: 0 to 1 - some pads report them as brake and gas)
        if (std::max(axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_BRAKE)) > kDeadzone)
            bits |= ButtonMapping[EmuButton_L2];
        if (std::max(axis(AMOTION_EVENT_AXIS_RTRIGGER), axis(AMOTION_EVENT_AXIS_GAS)) > kDeadzone)
            bits |= ButtonMapping[EmuButton_R2];
        if (bits)
            state->gamepadUsed = true;
        state->gamepadAxisBits = bits;
        return 1;
    }

    // A light vibration (MainActivity.haptic) - an on-screen button pressed.
    void Haptic(android_app *app)
    {
        JNIEnv *env = nullptr;
        app->activity->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        jclass activityClass = env->GetObjectClass(app->activity->clazz);
        jmethodID haptic = env->GetMethodID(activityClass, "haptic", "()V");
        env->DeleteLocalRef(activityClass);
        if (haptic)
            env->CallVoidMethod(app->activity->clazz, haptic);
    }

    // Something went wrong for good: say so on the screen (MainActivity.showError)
    // rather than leaving it black.
    void ShowError(android_app *app, const std::string &message)
    {
        LOGE("Phone app: %s", message.c_str());
        JNIEnv *env = nullptr;
        app->activity->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        jclass activityClass = env->GetObjectClass(app->activity->clazz);
        jmethodID show = env->GetMethodID(activityClass, "showError", "(Ljava/lang/String;)V");
        env->DeleteLocalRef(activityClass);
        if (!show)
        {
            env->ExceptionClear();
            return;
        }
        jstring text = env->NewStringUTF(message.c_str());
        env->CallVoidMethod(app->activity->clazz, show, text);
        env->DeleteLocalRef(text);
    }

    // After an error: keep answering Android until the player closes the app
    // (returning from android_main would leave a frozen black window).
    void WaitForClose(android_app *app)
    {
        while (!app->destroyRequested)
        {
            int events = 0;
            android_poll_source *source = nullptr;
            if (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void **>(&source)) >= 0 && source)
                source->process(app, source);
        }
    }

    // The Vulkan side of the window: surface and swapchain, made again
    // whenever Android hands over a new window (back from the background,
    // a rotation).
    struct Presenter
    {
        VulkanRenderer *renderer = nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        VkExtent2D extent{};
        std::vector<VkImage> images;
        uint64_t generation = 0;

        VkSurfaceKHR CreateSurface(VkInstance instance, ANativeWindow *window)
        {
            auto create = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
            if (!create)
                throw std::runtime_error("no vkCreateAndroidSurfaceKHR");
            VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
            info.window = window;
            CheckVk(create(instance, &info, nullptr, &surface), "vkCreateAndroidSurfaceKHR");
            return surface;
        }

        void ChooseFormat()
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->GetPhysicalDevice(), surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->GetPhysicalDevice(), surface, &count, formats.data());
            format = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
            for (const VkSurfaceFormatKHR &f : formats)
                if (f.format == VK_FORMAT_R8G8B8A8_SRGB || f.format == VK_FORMAT_B8G8R8A8_SRGB)
                {
                    format = f;
                    break;
                }
        }

        void CreateSwapchain(UiRenderer &ui, ANativeWindow *window)
        {
            VkDevice device = renderer->GetDevice();
            vkDeviceWaitIdle(device);
            VkSurfaceCapabilitiesKHR caps{};
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer->GetPhysicalDevice(), surface, &caps);
            // The window's own size: Android turns the picture for the
            // display itself (an identity transform).
            extent = {static_cast<uint32_t>(ANativeWindow_getWidth(window)), static_cast<uint32_t>(ANativeWindow_getHeight(window))};
            extent.width = std::clamp(extent.width, caps.minImageExtent.width, std::max(caps.minImageExtent.width, caps.maxImageExtent.width));
            extent.height = std::clamp(extent.height, caps.minImageExtent.height, std::max(caps.minImageExtent.height, caps.maxImageExtent.height));

            VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            info.surface = surface;
            info.minImageCount = std::max(2u, caps.minImageCount);
            if (caps.maxImageCount)
                info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
            info.imageFormat = format.format;
            info.imageColorSpace = format.colorSpace;
            info.imageExtent = extent;
            info.imageArrayLayers = 1;
            info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                                    : caps.currentTransform;
            info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                                                                                    : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
            info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            info.clipped = VK_TRUE;
            info.oldSwapchain = swapchain;
            VkSwapchainKHR fresh = VK_NULL_HANDLE;
            CheckVk(vkCreateSwapchainKHR(device, &info, nullptr, &fresh), "vkCreateSwapchainKHR");
            if (swapchain != VK_NULL_HANDLE)
                vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = fresh;
            ++generation;
            ui.InvalidateRenderTargets(); // (see pc2d's recreateSwapchain)
            uint32_t count = 0;
            vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
            images.resize(count);
            vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
        }

        void Destroy(UiRenderer &ui)
        {
            if (!renderer || renderer->GetDevice() == VK_NULL_HANDLE)
                return;
            vkDeviceWaitIdle(renderer->GetDevice());
            ui.InvalidateRenderTargets();
            if (swapchain != VK_NULL_HANDLE)
                vkDestroySwapchainKHR(renderer->GetDevice(), swapchain, nullptr);
            if (surface != VK_NULL_HANDLE)
                vkDestroySurfaceKHR(renderer->GetInstance(), surface, nullptr);
            swapchain = VK_NULL_HANDLE;
            surface = VK_NULL_HANDLE;
            images.clear();
        }
    };

    // As the PC window's: where the screen goes in a w x h area.
    void FitScreen(float areaX, float areaY, float w, float h, bool wholePixels, float &x, float &y, float &sw, float &sh)
    {
        const float fit = std::min(w / static_cast<float>(Emulator::kPreviewWidth), h / static_cast<float>(Emulator::kPreviewHeight));
        const float whole = std::floor(fit);
        const float scale = wholePixels && whole >= 1.0f ? whole : fit;
        sw = Emulator::kPreviewWidth * scale;
        sh = Emulator::kPreviewHeight * scale;
        x = areaX + std::floor((w - sw) / 2.0f);
        y = areaY + std::floor((h - sh) / 2.0f);
    }

    // SDK-backed fixed-view Cardboard presentation. The emulator's single
    // colorized SBS texture is drawn once into the two atlas halves, then the
    // official per-viewer meshes distort those halves into the display.
    // This class never creates a head tracker or reads device sensors.
    class CardboardOutput
    {
        struct DepthTarget
        {
            VkImage image{VK_NULL_HANDLE};
            VkDeviceMemory memory{VK_NULL_HANDLE};
            VkImageView view{VK_NULL_HANDLE};
        };

    public:
        void Ensure(UiRenderer &ui, VulkanRenderer &renderer, Presenter &presenter, AndroidPlatform &platform)
        {
            if (presenter.extent.width == 0 || presenter.extent.height == 0)
                throw std::runtime_error("Cardboard needs a non-empty landscape display");
            const uint32_t width = presenter.extent.width;
            const uint32_t height = presenter.extent.height;
            if (!m_atlas.IsValid())
                m_atlas = ui.CreateRenderTexture(width, height, VK_FORMAT_R8G8B8A8_SRGB, false, true);
            else
                ui.ResizeRenderTexture(m_atlas, width, height);

            const bool swapchainChanged = !m_displayPass || m_swapchainGeneration != presenter.generation;
            if (swapchainChanged)
            {
                DestroyDistortion(renderer.GetDevice());
                DestroyDisplayTargets(renderer.GetDevice());
                CreateDisplayTargets(renderer, presenter);
                m_swapchainGeneration = presenter.generation;
            }

            const int changedCount = CardboardQrCode_getDeviceParamsChangedCount();
            if (!m_lens || m_width != width || m_height != height || changedCount != m_paramsChangedCount || swapchainChanged)
                CreateDistortion(renderer, presenter, width, height, changedCount, platform);
        }

        UiImageHandle Atlas() const { return m_atlas; }
        bool IsActive() const { return m_displayPass != VK_NULL_HANDLE || m_distortion != nullptr || m_lens != nullptr; }

        void Distort(UiRenderer &ui, uint32_t imageIndex, uint32_t width, uint32_t height)
        {
            if (!m_distortion || imageIndex >= m_framebuffers.size())
                throw std::runtime_error("Cardboard distortion renderer is not ready");

            const VkImage atlasImage = ui.GetRenderTextureImage(m_atlas);
            if (atlasImage == VK_NULL_HANDLE)
                throw std::runtime_error("Cardboard eye texture is unavailable");
            m_leftEye.texture = reinterpret_cast<uint64_t>(atlasImage);
            m_rightEye.texture = m_leftEye.texture;

            ui.BeginCardboardFrame(m_framebuffers[imageIndex], m_displayPass, width, height,
                                   XrColor4f{0.0f, 0.0f, 0.0f, 1.0f});
            VkRenderPass renderPass = m_displayPass;
            VkCommandBuffer commandBuffer = ui.GetCommandBuffer();
            CardboardVulkanDistortionRendererTarget target{};
            target.vk_render_pass = reinterpret_cast<uint64_t>(&renderPass);
            target.vk_command_buffer = reinterpret_cast<uint64_t>(&commandBuffer);
            target.swapchain_image_index = imageIndex;
            CardboardDistortionRenderer_renderEyeToDisplay(
                m_distortion, reinterpret_cast<uint64_t>(&target), 0, 0, static_cast<int>(width), static_cast<int>(height),
                &m_leftEye, &m_rightEye);
            ui.EndFrame();
        }

        void Destroy(VkDevice device)
        {
            if (device != VK_NULL_HANDLE)
                vkDeviceWaitIdle(device);
            DestroyDistortion(device);
            DestroyDisplayTargets(device);
            m_swapchainGeneration = 0;
            m_width = m_height = 0;
        }

    private:
        void DestroyDistortion(VkDevice device)
        {
            if (m_distortion)
            {
                CardboardDistortionRenderer_destroy(m_distortion);
                m_distortion = nullptr;
            }
            if (m_lens)
            {
                CardboardLensDistortion_destroy(m_lens);
                m_lens = nullptr;
            }
            if (device != VK_NULL_HANDLE)
                vkDeviceWaitIdle(device);
        }

        void DestroyDisplayTargets(VkDevice device)
        {
            if (device != VK_NULL_HANDLE)
            {
                for (VkFramebuffer framebuffer : m_framebuffers)
                    if (framebuffer)
                        vkDestroyFramebuffer(device, framebuffer, nullptr);
                for (VkImageView view : m_colorViews)
                    if (view)
                        vkDestroyImageView(device, view, nullptr);
                for (DepthTarget &depth : m_depthTargets)
                {
                    if (depth.view)
                        vkDestroyImageView(device, depth.view, nullptr);
                    if (depth.image)
                        vkDestroyImage(device, depth.image, nullptr);
                    if (depth.memory)
                        vkFreeMemory(device, depth.memory, nullptr);
                }
                if (m_displayPass)
                    vkDestroyRenderPass(device, m_displayPass, nullptr);
            }
            m_framebuffers.clear();
            m_colorViews.clear();
            m_depthTargets.clear();
            m_displayPass = VK_NULL_HANDLE;
        }

        static uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t typeBits, VkMemoryPropertyFlags wanted)
        {
            VkPhysicalDeviceMemoryProperties properties{};
            vkGetPhysicalDeviceMemoryProperties(physical, &properties);
            for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
                if ((typeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
                    return i;
            throw std::runtime_error("No device-local Vulkan memory type for Cardboard depth buffer");
        }

        void CreateDisplayTargets(VulkanRenderer &renderer, const Presenter &presenter)
        {
            VkPhysicalDevice physical = renderer.GetPhysicalDevice();
            VkDevice device = renderer.GetDevice();
            VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
            VkFormatProperties formatProperties{};
            vkGetPhysicalDeviceFormatProperties(physical, depthFormat, &formatProperties);
            if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            {
                depthFormat = VK_FORMAT_D24_UNORM_S8_UINT;
                vkGetPhysicalDeviceFormatProperties(physical, depthFormat, &formatProperties);
            }
            if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
                throw std::runtime_error("This Vulkan device has no supported Cardboard depth format");
            m_depthFormat = depthFormat;

            VkAttachmentDescription attachments[2]{};
            attachments[0].format = presenter.format.format;
            attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            attachments[1].format = depthFormat;
            attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;
            subpass.pDepthStencilAttachment = &depthRef;
            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            passInfo.attachmentCount = 2;
            passInfo.pAttachments = attachments;
            passInfo.subpassCount = 1;
            passInfo.pSubpasses = &subpass;
            passInfo.dependencyCount = 1;
            passInfo.pDependencies = &dependency;
            if (vkCreateRenderPass(device, &passInfo, nullptr, &m_displayPass) != VK_SUCCESS)
                throw std::runtime_error("Couldn't create Cardboard Vulkan render pass");

            m_colorViews.reserve(presenter.images.size());
            m_depthTargets.resize(presenter.images.size());
            m_framebuffers.reserve(presenter.images.size());
            for (size_t i = 0; i < presenter.images.size(); ++i)
            {
                VkImageViewCreateInfo colorViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                colorViewInfo.image = presenter.images[i];
                colorViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                colorViewInfo.format = presenter.format.format;
                colorViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageView colorView = VK_NULL_HANDLE;
                if (vkCreateImageView(device, &colorViewInfo, nullptr, &colorView) != VK_SUCCESS)
                    throw std::runtime_error("Couldn't create Cardboard swapchain image view");
                m_colorViews.push_back(colorView);

                DepthTarget &depth = m_depthTargets[i];
                VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                imageInfo.imageType = VK_IMAGE_TYPE_2D;
                imageInfo.format = depthFormat;
                imageInfo.extent = {presenter.extent.width, presenter.extent.height, 1};
                imageInfo.mipLevels = 1;
                imageInfo.arrayLayers = 1;
                imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
                imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                if (vkCreateImage(device, &imageInfo, nullptr, &depth.image) != VK_SUCCESS)
                    throw std::runtime_error("Couldn't create Cardboard depth image");
                VkMemoryRequirements requirements{};
                vkGetImageMemoryRequirements(device, depth.image, &requirements);
                VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                allocation.allocationSize = requirements.size;
                allocation.memoryTypeIndex = FindMemoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (vkAllocateMemory(device, &allocation, nullptr, &depth.memory) != VK_SUCCESS ||
                    vkBindImageMemory(device, depth.image, depth.memory, 0) != VK_SUCCESS)
                    throw std::runtime_error("Couldn't allocate Cardboard depth image memory");
                VkImageViewCreateInfo depthViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                depthViewInfo.image = depth.image;
                depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                depthViewInfo.format = depthFormat;
                depthViewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                if (vkCreateImageView(device, &depthViewInfo, nullptr, &depth.view) != VK_SUCCESS)
                    throw std::runtime_error("Couldn't create Cardboard depth image view");
                const VkImageView views[] = {colorView, depth.view};
                VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                framebufferInfo.renderPass = m_displayPass;
                framebufferInfo.attachmentCount = 2;
                framebufferInfo.pAttachments = views;
                framebufferInfo.width = presenter.extent.width;
                framebufferInfo.height = presenter.extent.height;
                framebufferInfo.layers = 1;
                VkFramebuffer framebuffer = VK_NULL_HANDLE;
                if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS)
                    throw std::runtime_error("Couldn't create Cardboard display framebuffer");
                m_framebuffers.push_back(framebuffer);
            }
        }

        void CreateDistortion(VulkanRenderer &renderer, const Presenter &presenter, uint32_t width, uint32_t height,
                              int changedCount, AndroidPlatform &platform)
        {
            DestroyDistortion(renderer.GetDevice());
            uint8_t *params = nullptr;
            int size = 0;
            CardboardQrCode_getSavedDeviceParams(&params, &size);
            bool ownedParams = true;
            const bool usingDefault = size <= 0 || params == nullptr;
            if (usingDefault)
            {
                if (params)
                    CardboardQrCode_destroy(params);
                params = nullptr;
                CardboardQrCode_getCardboardV1DeviceParams(&params, &size);
                ownedParams = false;
            }
            if (!params || size <= 0)
                throw std::runtime_error("Cardboard couldn't load a viewer profile");

            CardboardLensDistortion *lens = CardboardLensDistortion_create(params, size, static_cast<int>(width),
                                                                           static_cast<int>(height));
            if (ownedParams)
                CardboardQrCode_destroy(params);
            if (!lens)
                throw std::runtime_error("Cardboard couldn't create optical calibration for this display");

            VkPhysicalDevice physical = renderer.GetPhysicalDevice();
            VkDevice device = renderer.GetDevice();
            VkSwapchainKHR swapchain = presenter.swapchain;
            CardboardVulkanDistortionRendererConfig config{};
            config.physical_device = reinterpret_cast<uint64_t>(&physical);
            config.logical_device = reinterpret_cast<uint64_t>(&device);
            config.vk_swapchain = reinterpret_cast<uint64_t>(&swapchain);
            CardboardDistortionRenderer *distortion = CardboardVulkanDistortionRenderer_create(&config);
            if (!distortion)
            {
                CardboardLensDistortion_destroy(lens);
                throw std::runtime_error("Cardboard couldn't create its Vulkan distortion renderer");
            }

            CardboardMesh leftMesh{}, rightMesh{};
            CardboardLensDistortion_getDistortionMesh(lens, kLeft, &leftMesh);
            CardboardLensDistortion_getDistortionMesh(lens, kRight, &rightMesh);
            if (!leftMesh.vertices || !rightMesh.vertices || leftMesh.n_indices <= 0 || rightMesh.n_indices <= 0)
            {
                CardboardDistortionRenderer_destroy(distortion);
                CardboardLensDistortion_destroy(lens);
                throw std::runtime_error("Cardboard returned an empty lens distortion mesh");
            }
            CardboardDistortionRenderer_setMesh(distortion, &leftMesh, kLeft);
            CardboardDistortionRenderer_setMesh(distortion, &rightMesh, kRight);

            m_lens = lens;
            m_distortion = distortion;
            m_width = width;
            m_height = height;
            m_paramsChangedCount = changedCount;
            m_leftEye.left_u = 0.0f;
            m_leftEye.right_u = 0.5f;
            m_leftEye.top_v = 1.0f;
            m_leftEye.bottom_v = 0.0f;
            m_rightEye = m_leftEye;
            m_rightEye.left_u = 0.5f;
            m_rightEye.right_u = 1.0f;
            if (usingDefault && !m_defaultProfileNoticeShown)
            {
                platform.ShowCardboardMessage("Using Cardboard's default viewer profile. Scan your viewer QR code for accurate optics.");
                m_defaultProfileNoticeShown = true;
            }
        }

        UiImageHandle m_atlas;
        CardboardLensDistortion *m_lens = nullptr;
        CardboardDistortionRenderer *m_distortion = nullptr;
        CardboardEyeTextureDescription m_leftEye{}, m_rightEye{};
        VkRenderPass m_displayPass{VK_NULL_HANDLE};
        VkFormat m_depthFormat{VK_FORMAT_UNDEFINED};
        std::vector<VkImageView> m_colorViews;
        std::vector<DepthTarget> m_depthTargets;
        std::vector<VkFramebuffer> m_framebuffers;
        uint64_t m_swapchainGeneration = 0;
        uint32_t m_width = 0, m_height = 0;
        int m_paramsChangedCount = -1;
        bool m_defaultProfileNoticeShown = false;
    };
} // namespace

void RunPhoneApp(android_app *app, AndroidPlatform &platform)
{
    platform.SetPhoneMode(true);
    if (!platform.InitializeCardboardSdk())
        platform.ShowCardboardMessage("Google Cardboard isn't available on this launch. The normal phone display will still work.");
    PhoneState state;
    app->userData = &state;
    platform.SetCanQuit(true); // (the menu's Quit - a TV with a gamepad has no other way out)
    app->onAppCmd = HandleAppCmd;
    app->onInputEvent = HandleInputEvent;
    // (no dimming while playing with a gamepad)
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);

    VulkanRenderer renderer;
    UiRenderer uiRenderer;
    // On the heap: the emulator is ~700 KB, and this thread's stack (the
    // native activity's, 1 MB) also holds android_main's frame - on the
    // stack it overflowed the moment this function was entered.
    auto emulatorOnHeap = std::make_unique<Emulator>();
    Emulator &emulator = *emulatorOnHeap;
    AppMenu appMenu;
    AppSettings settings;
    TouchControls touch;
    // No game loaded: the menu inside a Virtual Boy (faded out with the menu when a game starts).
    VirtualBoyFrame vbFrame;
    float vbFrameAlpha = 1.0f;
    Presenter presenter;
    presenter.renderer = &renderer;
    CardboardOutput cardboard;
    // What already happened before this function took the events over (the
    // VR app falls back here when the headset runtime can't start).
    state.resumed = app->activityState == APP_CMD_RESUME;
    state.window = app->window;
    state.windowChanged = app->window != nullptr;
    state.releaseWindow = [&]()
    {
        cardboard.Destroy(renderer.GetDevice());
        presenter.Destroy(uiRenderer);
    };
    bool ready = false; // the device, the renderer, the menu - once
    VkFence acquireFence = VK_NULL_HANDLE;
    bool settingsLoaded = false;
    bool initialCardboardSettingsPendingSave = false;
    bool editRequested = false; // Settings > Touch controls > Edit
    std::string editGame;       // ...a game's own (Library: hold its card), or ""

    uint32_t buttonStates[3]{}, lastButtonStates[3]{};
    bool menuComboWasHeld = false, backWasHeld = false;
    std::string shownRom;
    auto lastFrameTime = std::chrono::steady_clock::now();
    bool fatal = false;
    float hintedFps = -1.0f;
    ANativeWindow *hintedWindow = nullptr;

    for (;;)
    {
      try
      {
        // Events: all that are waiting - or, with nothing to show (in the
        // background, no window, the ROMs folder picker up), wait for one.
        // The Cardboard Java initializer is queued onto Android's main thread;
        // briefly poll while it completes so SDK class lookup never blocks the
        // UI thread during NativeActivity.onCreate().
        bool quit = false;
        for (;;)
        {
            // (the folder is asked of Android only until the app is up - it's a
            // system call)
            const bool sdkInitComplete = platform.CardboardSdkInitializationComplete();
            const bool active = state.resumed && state.window && (ready || sdkInitComplete);
            int events = 0;
            android_poll_source *source = nullptr;
            const int timeoutMs = active ? 0 : (sdkInitComplete ? -1 : 100);
            if (ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void **>(&source)) < 0)
                break;
            const bool wasResumed = state.resumed;
            if (source)
                source->process(app, source);
            if (app->destroyRequested)
            {
                quit = true;
                break;
            }
            // The window is going away: let go of it now, while it still
            // exists. Gone to the background: keep the game's save safe
            // (Android may close the app there without a word).
            if (state.windowChanged && !state.window)
            {
                cardboard.Destroy(renderer.GetDevice());
                presenter.Destroy(uiRenderer);
            }
            if (wasResumed && !state.resumed && ready)
                emulator.SaveRamNow();
        }
        if (quit)
            break;

        // Wait asynchronously for the UI-thread Cardboard setup before making
        // settings visible. A failed or timed-out setup still opens ordinary
        // phone mode once the Platform marks initialization complete.
        if (!ready && !platform.CardboardSdkInitializationComplete())
            continue;

        if (state.windowChanged)
        {
            state.windowChanged = false;
            try
            {
                cardboard.Destroy(renderer.GetDevice());
                presenter.Destroy(uiRenderer);
                if (state.window)
                {
                    if (!ready)
                    {
                        const std::vector<const char *> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
                        VkInstance instance = renderer.CreateInstanceStandalone(extensions);
                        presenter.CreateSurface(instance, state.window);
                        renderer.CreateDeviceForSurface(presenter.surface);
                    }
                    else
                        presenter.CreateSurface(renderer.GetInstance(), state.window);
                    if (!ready)
                        presenter.ChooseFormat();
                    presenter.CreateSwapchain(uiRenderer, state.window);
                    if (!ready)
                    {
                        if (!settingsLoaded)
                        {
                            const bool hasSavedSettings = settings.Load(platform);
                            // Cardboard is the useful first-run display on a
                            // phone. Existing settings always win, so players
                            // who chose 2D or another stereo layout keep it.
                            if (!hasSavedSettings && platform.SupportsCardboardViewer())
                            {
                                settings.screen3D = static_cast<int>(Screen3D::GoogleCardboard);
                                initialCardboardSettingsPendingSave = true;
                            }
                            if (!platform.SupportsCardboardViewer() &&
                                static_cast<Screen3D>(settings.screen3D) == Screen3D::GoogleCardboard)
                            {
                                settings.screen3D = static_cast<int>(Screen3D::Off);
                                settings.Save(platform);
                            }
                            // (A/B where the Virtual Boy has them, as on the PC window)
                            if (!settings.gamepadByPosition)
                            {
                                ButtonMapper::MigrateGamepadAToPosition(settings.vbButtons);
                                settings.gamepadByPosition = true;
                            }
                            ButtonMapper::ApplyDefaultGamepadBindings(settings.vbButtons, true);
                            settingsLoaded = true;
                        }
                        uiRenderer.Initialize(renderer.GetDevice(), renderer.GetPhysicalDevice(), renderer.GetQueue(),
                                              renderer.GetQueueFamilyIndex(), renderer.GetCommandPool(), renderer.GetCommandBuffer());
                        emulator.Initialize(uiRenderer, platform);
                        appMenu.Initialize(uiRenderer, presenter.format.format, emulator, settings, platform,
                                           ButtonMappingProfile::Phone);
                        touch.Initialize(uiRenderer, platform);
                        appMenu.SetEditTouchControls(
                            [&editRequested, &editGame](const std::string &game)
                            {
                                editGame = game;
                                editRequested = true;
                            },
                            [&touch, &platform](const std::string &game) { touch.ForgetGameLayout(platform, game); });
                        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                        CheckVk(vkCreateFence(renderer.GetDevice(), &fenceInfo, nullptr, &acquireFence), "vkCreateFence");
                        ready = true;
                        LOGI("Phone app running (%ux%u)", presenter.extent.width, presenter.extent.height);
                    }
                }
            }
            catch (const std::exception &ex)
            {
                throw std::runtime_error(std::string("Couldn't start: ") + ex.what());
            }
        }
        if (!ready || presenter.swapchain == VK_NULL_HANDLE || !state.resumed)
            continue;

        // The SAF folder picker can finish after the native window is ready.
        // Save the first-run Cardboard choice as soon as Android grants a ROM
        // folder, without overwriting existing display preferences.
        if (initialCardboardSettingsPendingSave && platform.HasRomsFolder())
        {
            settings.Save(platform);
            initialCardboardSettingsPendingSave = false;
        }

        // The window may have changed size without a new one (a resize).
        if (static_cast<uint32_t>(ANativeWindow_getWidth(state.window)) != presenter.extent.width ||
            static_cast<uint32_t>(ANativeWindow_getHeight(state.window)) != presenter.extent.height)
        {
            cardboard.Destroy(renderer.GetDevice());
            presenter.CreateSwapchain(uiRenderer, state.window);
        }

        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

        const float w = static_cast<float>(presenter.extent.width), h = static_cast<float>(presenter.extent.height);
        const int32_t density = AConfiguration_getDensity(app->config);
        const float dp = (density > 0 && density < 0xfffe ? density : 160) / 160.0f;

        // Editing the touch controls (asked for in the menu): the game waits,
        // the controls show, the menu comes back when the player is done.
        if (editRequested && static_cast<Screen3D>(settings.screen3D) == Screen3D::GoogleCardboard)
        {
            editRequested = false;
            platform.ShowCardboardMessage("Touch controls are hidden in Cardboard mode. Use a gamepad or Android Back for the menu.");
        }
        if (editRequested)
        {
            editRequested = false;
            appMenu.Hide();
            touch.BeginEdit(platform, editGame);
        }
        // (the game playing's own layout, if it has one)
        touch.SetGame(platform, emulator.RomName());
        if (static_cast<Screen3D>(settings.screen3D) == Screen3D::GoogleCardboard && touch.Editing())
        {
            touch.EndEdit();
            touch.Save(platform);
        }
        const bool cardboardRequested = static_cast<Screen3D>(settings.screen3D) == Screen3D::GoogleCardboard;
        const bool editing = touch.Editing();

        // The touch controls: hidden while a gamepad plays, back on a touch.
        // Switched off in Settings, they start hidden, show when the screen is
        // touched and go again a few seconds after the last touch.
        {
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            static bool startShown = false;
            if (!startShown)
            {
                startShown = true;
                state.touchShown = settings.touchControls;
            }
            static bool lastSetting = settings.touchControls;
            if (settings.touchControls != lastSetting) // (just switched in Settings)
            {
                lastSetting = settings.touchControls;
                state.touchShown = settings.touchControls;
            }
            if (state.touched || !state.fingers.empty())
                state.lastTouchSeconds = now;
            if (state.gamepadUsed)
                state.touchShown = false;
            else if (state.touched)
                state.touchShown = true;
            else if (!settings.touchControls && state.touchShown && state.fingers.empty() && now - state.lastTouchSeconds > 4.0)
                state.touchShown = false;
            touch.SetVisible(!cardboardRequested && (editing || state.touchShown));
            touch.SetOverGame(!cardboardRequested && !settings.touchControls && !editing); // (the picture stays put as they come and go)
        }
        state.touched = false;
        if (emulator.RomName() != shownRom)
            shownRom = emulator.RomName();
        // (editing a game's own layout: that game's controls - its right D-pad or not)
        touch.Layout(w, h, dp, TouchControls::UsesRightDpad(editing && !touch.LayoutGame().empty() ? touch.LayoutGame() : shownRom));

        // The menu: as big as fits (not whole multiples: a phone's screen is
        // much wider than the menu - this fills its height), its shape kept.
        const float menuWidth = cardboardRequested ? std::floor(w / 2.0f) : w;
        const float menuScale = std::max(1.0f, std::min(menuWidth / static_cast<float>(kMenuWidth), h / static_cast<float>(kMenuHeight)));
        appMenu.SetMenuScale(uiRenderer, menuScale);
        const float menuX = cardboardRequested ? 0.0f : std::floor((w - kMenuWidth * menuScale) / 2.0f);
        const float menuY = std::floor((h - kMenuHeight * menuScale) / 2.0f);

        std::vector<TouchControls::Touch> touches;
        for (const auto &[id, f] : state.fingers)
            touches.push_back({id, f.x, f.y});

        // A finger in the menu: tap to pick, drag to scroll (the first one).
        // (Not while it fades out for the touch editor: those fingers are the editor's.)
        if (appMenu.IsVisible() && !editing)
        {
            const Finger *first = state.fingers.empty() ? nullptr : &state.fingers.begin()->second;
            float pointerMenuX = menuX;
            float pointerMenuY = menuY;
            bool pointerAvailable = first != nullptr;
            if (cardboardRequested)
            {
                // The menu is a duplicated plane. Allow taps on the left-eye
                // copy when the phone is outside the viewer, while the right
                // copy and the hidden gameplay overlay remain non-interactive.
                const float eyeWidth = std::floor(w / 2.0f);
                pointerMenuX = std::floor((eyeWidth - kMenuWidth * menuScale) / 2.0f);
                pointerMenuY = std::floor((h - kMenuHeight * menuScale) / 2.0f);
                pointerAvailable = first && first->x >= pointerMenuX && first->x <= pointerMenuX + kMenuWidth * menuScale &&
                                   first->x < eyeWidth && first->y >= pointerMenuY &&
                                   first->y <= pointerMenuY + kMenuHeight * menuScale;
            }
            const float mx = pointerAvailable ? (first->x - pointerMenuX) / menuScale : 0.0f;
            const float my = pointerAvailable ? (first->y - pointerMenuY) / menuScale : 0.0f;
            appMenu.SetPointer(pointerAvailable, mx, my, pointerAvailable && !first->up, 0.0f, false, true);
        }
        else
            appMenu.SetPointer(false, 0, 0, false);

        if (editing)
        {
            // (the menu still fades out - only its Update moves that along;
            // without it the menu stayed on screen, frozen, over the editor)
            uint32_t noButtons[3] = {}, noLastButtons[3] = {};
            appMenu.Update(noButtons, noLastButtons, deltaSeconds);
            const bool backTapped = state.backTapped || (state.backHeld && !backWasHeld);
            state.backTapped = false;
            backWasHeld = state.backHeld;
            if (touch.UpdateEdit(touches) || backTapped)
            {
                touch.EndEdit();
                touch.Save(platform);
                appMenu.Show();
            }
        }
        else
        {
        // Menu buttons: the gamepad's, and Back as the menu's B.
        std::memcpy(lastButtonStates, buttonStates, sizeof(buttonStates));
        buttonStates[ButtonMapper::DeviceGamepad] = state.gamepadKeyBits | state.gamepadAxisBits;
        buttonStates[ButtonMapper::DeviceLeftTouch] = 0;
        const bool backTapped = state.backTapped || (state.backHeld && !backWasHeld);
        state.backTapped = false;
        buttonStates[ButtonMapper::DeviceRightTouch] =
            (state.backHeld || backTapped) && appMenu.IsOpen() ? ButtonMapper::ButtonMapping[ButtonMapper::EmuButton_B] : 0;
        const bool menuWasOpen = appMenu.IsOpen();
        appMenu.Update(buttonStates, lastButtonStates, deltaSeconds);

        // Opening the menu: Back, the gamepad's Guide or Select + Start, the
        // on-screen menu button.
        const uint32_t selectStart = ButtonMapper::ButtonMapping[ButtonMapper::EmuButton_Back] |
                                     ButtonMapper::ButtonMapping[ButtonMapper::EmuButton_Enter];
        const bool combo = (state.gamepadKeyBits & selectStart) == selectStart || state.gamepadGuideHeld;
        if (combo && !menuComboWasHeld)
            appMenu.ToggleOpen();
        menuComboWasHeld = combo;
        if (cardboardRequested && backTapped)
        {
            // Android Back is a dedicated physical escape hatch while the
            // phone is in a viewer. It returns to the ordinary display even
            // if the on-screen menu is open.
            settings.screen3D = static_cast<int>(Screen3D::Off);
            settings.Save(platform);
            appMenu.Hide();
            platform.ShowCardboardMessage("Google Cardboard mode off");
        }
        else if (backTapped && !menuWasOpen && !appMenu.IsOpen() && emulator.HasGame())
            appMenu.Show();
        backWasHeld = state.backHeld;

        // "Back to game" beside the menu (Back works too - not every phone
        // hands the back gesture over in full screen).
        if (appMenu.IsVisible() && emulator.HasGame() && !cardboardRequested)
        {
            touch.LayoutResume(menuX + kMenuWidth * menuScale, menuY);
            if (touch.UpdateResume(touches))
                appMenu.Hide();
        }

        // Gamepad buttons for the mapping page.
        for (int bit = 0; bit < ButtonMapper::EmuButtonCount; ++bit)
        {
            const uint32_t mask = ButtonMapper::ButtonMapping[bit];
            if ((buttonStates[ButtonMapper::DeviceGamepad] & mask) && !(lastButtonStates[ButtonMapper::DeviceGamepad] & mask))
                appMenu.SubmitRawMappingInput({true, ButtonMapper::DeviceGamepad, bit});
        }

        emulator.SetShadePalette(settings.EffectiveShadePalette());
        bool menuPressed = false;
        const uint32_t touchBits = (appMenu.IsVisible() || cardboardRequested) ? 0 : touch.Update(touches, menuPressed);
        if (menuPressed && emulator.HasGame())
            appMenu.Show();
        if (touch.NewlyPressed())
            Haptic(app);
        if (!appMenu.IsOpen())
        {
            // (only the gamepad's buttons go through the mapping - the other
            // slots are the menu's)
            // (the left stick moves like the D-pad, the triggers press like the bumpers)
            uint32_t gameplay[3] = {ButtonMapper::GamepadCompanionBits(buttonStates[ButtonMapper::DeviceGamepad]), 0, 0};
            appMenu.ApplyGameplayInputSuppression(gameplay);
            emulator.SetGameplayInput(ButtonMapper::TranslateToVBBitmask(gameplay, appMenu.Controls().Buttons(settings)) | touchBits);
            emulator.RunFrame(deltaSeconds);
        }
        }

        // (the lifted fingers have been seen)
        for (auto it = state.fingers.begin(); it != state.fingers.end();)
            it = it->second.up ? state.fingers.erase(it) : std::next(it);

        const bool renderCardboardMenu = static_cast<Screen3D>(settings.screen3D) == Screen3D::GoogleCardboard;
        const float renderMenuWidth = renderCardboardMenu ? std::floor(w / 2.0f) : w;
        const float renderMenuScale = std::max(1.0f, std::min(renderMenuWidth / static_cast<float>(kMenuWidth),
                                                             h / static_cast<float>(kMenuHeight)));
        appMenu.SetMenuScale(uiRenderer, renderMenuScale);
        const float renderMenuX = renderCardboardMenu ? 0.0f : std::floor((w - kMenuWidth * renderMenuScale) / 2.0f);
        const float renderMenuY = std::floor((h - kMenuHeight * renderMenuScale) / 2.0f);

        // (a game being played: 50 fps - the menu keeps the screen's own rate)
        const float wantedFps = emulator.HasGame() && !appMenu.IsVisible() && !editing ? 50.0f : 0.0f;
        if (wantedFps != hintedFps || state.window != hintedWindow)
        {
            hintedFps = wantedFps;
            hintedWindow = state.window;
            HintFrameRate(state.window, wantedFps);
        }

        if (appMenu.NeedsDraw())
            appMenu.RenderToBuffer(uiRenderer);
        vbFrameAlpha = !editing && !emulator.HasGame() ? 1.0f : std::min(vbFrameAlpha, appMenu.GetVisibility());
        if (vbFrameAlpha > 0.0f)
            vbFrame.Update(uiRenderer, w, h);
        const int anaglyph = AnaglyphOf(settings.screen3D);
        emulator.PrepareScreen(uiRenderer, presenter.format.format, settings.ScreenTint(), settings.ScreenPattern(),
                               settings.screenLook, anaglyph);

        bool useCardboard = static_cast<Screen3D>(settings.screen3D) == Screen3D::GoogleCardboard;
        if (useCardboard)
        {
            try
            {
                cardboard.Ensure(uiRenderer, renderer, presenter, platform);
            }
            catch (const std::exception &ex)
            {
                LOGE("Cardboard initialization failed: %s", ex.what());
                cardboard.Destroy(renderer.GetDevice());
                settings.screen3D = static_cast<int>(Screen3D::Off);
                settings.Save(platform);
                platform.ShowCardboardMessage("Cardboard could not start. Switched back to the normal phone display.");
                useCardboard = false;
            }
        }
        else if (cardboard.IsActive())
            cardboard.Destroy(renderer.GetDevice());

        vkResetFences(renderer.GetDevice(), 1, &acquireFence);
        uint32_t imageIndex = 0;
        const VkResult acquired = vkAcquireNextImageKHR(renderer.GetDevice(), presenter.swapchain, UINT64_MAX, VK_NULL_HANDLE,
                                                        acquireFence, &imageIndex);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            cardboard.Destroy(renderer.GetDevice());
            presenter.CreateSwapchain(uiRenderer, state.window);
            continue;
        }
        if (acquired == VK_ERROR_SURFACE_LOST_KHR)
        {
            state.windowChanged = true; // (a new surface for the same window)
            continue;
        }
        if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT)
            continue;
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            CheckVk(acquired, "vkAcquireNextImageKHR");
        vkWaitForFences(renderer.GetDevice(), 1, &acquireFence, VK_TRUE, UINT64_MAX);

        if (useCardboard)
            uiRenderer.BeginOffscreenFrame(cardboard.Atlas(), appMenu.GetBackgroundColor());
        else
            uiRenderer.BeginFrame(presenter.images[imageIndex], presenter.format.format, presenter.extent.width,
                                  presenter.extent.height, appMenu.GetBackgroundColor(), true);
        if (emulator.HasScreen() && emulator.HasGame())
        {
            float ax = 0, ay = 0, aw = 0, ah = 0;
            const Screen3D mode = static_cast<Screen3D>(settings.screen3D);
            float sx = 0, sy = 0, sw = 0, sh = 0;
            if (useCardboard)
            {
                const float eyeWidth = std::floor(w / 2.0f);
                for (int eye = 0; eye < 2; ++eye)
                {
                    const float areaX = eye * eyeWidth;
                    // Keep user magnification inside this eye's atlas half;
                    // otherwise an enlarged screen can leak into the other eye.
                    uiRenderer.SetClipRect(areaX, 0.0f, eyeWidth, h);
                    FitScreen(areaX, 0.0f, eyeWidth, h, settings.screenWholePixels, sx, sy, sw, sh);
                    sw *= settings.cardboardScale;
                    sh *= settings.cardboardScale;
                    sx = areaX + std::floor((eyeWidth - sw) / 2.0f);
                    sy = std::floor((h - sh) / 2.0f);
                    emulator.DrawScreen(uiRenderer, sx, sy, sw, sh, eye == 0 ? Emulator::Eye::Left : Emulator::Eye::Right,
                                        settings.ScreenTint(), settings.ScreenPattern(), settings.screenLook);
                }
                uiRenderer.ResetClipRect();
            }
            else if (mode == Screen3D::SideBySideHalf || mode == Screen3D::SideBySideFull || mode == Screen3D::CrossEyed ||
                mode == Screen3D::TopBottom || mode == Screen3D::TopBottomFull)
            {
                // A phone viewer or a 3D screen: the whole screen, one eye per half.
                const bool topBottom = mode == Screen3D::TopBottom || mode == Screen3D::TopBottomFull;
                const bool half = mode == Screen3D::SideBySideHalf || mode == Screen3D::TopBottom;
                const bool cross = mode == Screen3D::CrossEyed;
                for (int eye = 0; eye < 2; ++eye)
                {
                    const float ox = topBottom ? 0.0f : eye * std::floor(w / 2.0f), oy = topBottom ? eye * std::floor(h / 2.0f) : 0.0f;
                    if (half)
                    {
                        FitScreen(0, 0, w, h, settings.screenWholePixels, sx, sy, sw, sh);
                        sx = topBottom ? sx : sx / 2.0f;
                        sw = topBottom ? sw : sw / 2.0f;
                        sy = topBottom ? sy / 2.0f : sy;
                        sh = topBottom ? sh / 2.0f : sh;
                    }
                    else if (topBottom)
                        FitScreen(0, 0, w, std::floor(h / 2.0f), settings.screenWholePixels, sx, sy, sw, sh);
                    else
                        FitScreen(0, 0, std::floor(w / 2.0f), h, settings.screenWholePixels, sx, sy, sw, sh);
                    const Emulator::Eye which = (eye == 0) != cross ? Emulator::Eye::Left : Emulator::Eye::Right;
                    emulator.DrawScreen(uiRenderer, ox + sx, oy + sy, sw, sh, which, settings.ScreenTint(), settings.ScreenPattern(),
                                        settings.screenLook);
                }
            }
            else
            {
                touch.ScreenArea(ax, ay, aw, ah);
                FitScreen(ax, ay, aw, ah, settings.screenWholePixels, sx, sy, sw, sh);
                if (anaglyph > 0)
                    emulator.DrawAnaglyph(uiRenderer, sx, sy, sw, sh, anaglyph, settings.screenLook);
                else
                    emulator.DrawScreen(uiRenderer, sx, sy, sw, sh, Emulator::Eye::Left, settings.ScreenTint(), settings.ScreenPattern(),
                                        settings.screenLook);
            }
        }
        if (!useCardboard && vbFrameAlpha > 0.0f)
            vbFrame.Draw(uiRenderer, w, h, vbFrameAlpha);
        if (useCardboard)
        {
            const float eyeWidth = std::floor(w / 2.0f);
            const float cardMenuScale = renderMenuScale;
            if (appMenu.IsVisible() || appMenu.NoticeShowing())
                for (int eye = 0; eye < 2; ++eye)
                {
                    const float x = eye * eyeWidth + std::floor((eyeWidth - kMenuWidth * cardMenuScale) / 2.0f);
                    const float y = std::floor((h - kMenuHeight * cardMenuScale) / 2.0f);
                    appMenu.Draw(uiRenderer, x, y);
                }
        }
        else if (editing)
        {
            touch.DrawEdit(uiRenderer);
            if (appMenu.IsVisible()) // (fading out)
                appMenu.Draw(uiRenderer, renderMenuX, renderMenuY);
        }
        else if (appMenu.IsVisible())
        {
            appMenu.Draw(uiRenderer, renderMenuX, renderMenuY);
            if (emulator.HasGame())
                touch.DrawResume(uiRenderer);
        }
        else
        {
            touch.Draw(uiRenderer);
            if (appMenu.NoticeShowing()) // (an achievement unlocked, over the game)
                appMenu.Draw(uiRenderer, renderMenuX, renderMenuY);
        }
        uiRenderer.EndFrame();
        if (useCardboard)
            cardboard.Distort(uiRenderer, imageIndex, presenter.extent.width, presenter.extent.height);

        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &presenter.swapchain;
        presentInfo.pImageIndices = &imageIndex;
        // (not on VK_SUBOPTIMAL_KHR: Android says that on every frame while the
        // phone is turned sideways - the picture isn't pre-rotated, the system
        // turns it - and a new swapchain each frame dropped every picture
        // before it was shown: a black screen. A new size is caught above.)
        const VkResult presented = vkQueuePresentKHR(renderer.GetQueue(), &presentInfo);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR)
        {
            cardboard.Destroy(renderer.GetDevice());
            presenter.CreateSwapchain(uiRenderer, state.window);
        }
      }
      catch (const std::exception &ex)
      {
          ShowError(app, ex.what());
          fatal = true;
          break;
      }
    }

    state.releaseWindow = nullptr;
    if (fatal)
    {
        if (ready)
            emulator.SaveRamNow();
        cardboard.Destroy(renderer.GetDevice());
        presenter.Destroy(uiRenderer);
        WaitForClose(app);
    }
    if (ready)
    {
        vkDeviceWaitIdle(renderer.GetDevice());
        emulator.Shutdown();
        if (acquireFence != VK_NULL_HANDLE)
            vkDestroyFence(renderer.GetDevice(), acquireFence, nullptr);
        cardboard.Destroy(renderer.GetDevice());
        presenter.Destroy(uiRenderer);
        uiRenderer.Shutdown();
        renderer.Shutdown();
    }
}
