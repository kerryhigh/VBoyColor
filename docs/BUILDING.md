# Building VBoy Color

One CMake project builds every version:

| Target | What | Platforms |
|---|---|---|
| `VBoyColor` | the desktop app - a window, no headset | Windows, Linux |
| `VBoyColorVR` | desktop VR - OpenXR, streamed to a headset | Windows |
| `libvboycolor.so` | the Android app - VR on a Quest, the phone app (touch controls) on phones, tablets and handhelds; built through Gradle in `android/` | Android (arm64) |
| `vboycolor_libretro` | the RetroArch core - its own CMake project in `platform/libretro/` (see [RetroArch core](#retroarch-core)) | Windows, Linux, macOS, Android |

No Vulkan SDK is needed anywhere: Vulkan is loaded at runtime through `volk`,
and the shaders are compiled by glslang's C++ API, fetched and built as part
of the project. CMake fetches its other dependencies (Vulkan-Headers, volk,
OpenXR SDK, FreeType, GLFW) on the first configure. The emulator core and
Google Cardboard SDK are pinned git submodules:

```
git clone --recursive https://github.com/yuk27/VBoyColor
# or, in an existing clone:
git submodule update --init --recursive
```

Every push is also built by GitHub Actions (`.github/workflows/build.yml`):
Windows (both desktop apps), Linux (desktop), Android, and the RetroArch
core for Windows, Linux x86-64 and ARM64, macOS and Android. A tag `v*` makes a
GitHub release with all of them attached.

## Project layout

```
CMakeLists.txt        root build
core/                 shared, platform-independent app code
  app/                OpenXR instance/session/swapchains, event loop
  gfx/                Vulkan renderer, UI renderer, fonts, icons;
                      generated_shaders/ (SPIR-V headers, committed - see below)
  emu/                emulator wrapper (Beetle VB through libretro), save
                      states, audio, coloring (ShadeColorizer, AutoColors,
                      TileColorPack, ColorPackRenderer, tile tracking)
  menu/               the menu (ROM list, settings, button mapping, about)
  input/, io/         input mapping; settings, Platform interface
platform/             entry points, one per platform
  pc2d/Main.cpp       desktop app (GLFW window)
  pc/Main.cpp         desktop VR (OpenXR)
  android/            Android (NativeActivity): Quest VR, and the phone app (PhoneMain.cpp)
  desktop/            file access shared by both desktop apps
  libretro/           the RetroArch core (its own CMake project)
shaders/              GLSL sources
assets/runtime/       fonts and icon atlases the app loads
third_party/          Beetle VB (submodule), stb, miniaudio
cmake/                build helpers - PatchBeetleVip.cmake hooks tile tracking
                      into a generated copy of the core's video code
tools/                shader compiler, color pack tools, test harness
android/              Gradle project (externalNativeBuild -> root CMakeLists.txt)
```

## Windows

Visual Studio 2022 or newer (any edition, "Desktop development with C++"):

```
cmake -B build-pc -A x64 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-pc --config Release
build-pc\Release\VBoyColor.exe
```

The fonts and icons are embedded in the exe; next to it only the `roms`
folder is needed. A **Debug** build reads ROMs from `<repo>/sd/roms` instead,
for quick local testing.

`VBoyColorVR.exe` (same build) needs an OpenXR runtime and a connected headset
(Virtual Desktop, SteamVR or Quest Link). `XR_ERROR_FORM_FACTOR_UNAVAILABLE`
means the headset isn't connected right now. `tools/package_pc.ps1` builds it
and zips it with its `roms` folder.

## Linux

```
sudo apt install build-essential cmake ninja-build git \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libvulkan1
cmake -B build-pc -G Ninja -DCMAKE_BUILD_TYPE=Release -DGLFW_BUILD_WAYLAND=OFF
cmake --build build-pc --target VBoyColor
cd build-pc && ./VBoyColor
```

The build copies `fonts/` and `icons/` next to the executable. Any Vulkan 1.1
driver works (Mesa's lavapipe too, without a GPU).

## Android (Quest, phones and tablets)

Needs the Android SDK (platform 35, build-tools 35.0.0), NDK 29.0.14206865,
CMake 3.22.1 and a JDK 17+ (Android Studio's own works:
`JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"`).

Point `android/local.properties` at the SDK (or set `ANDROID_HOME`):

```
sdk.dir=C\:/Users/<you>/AppData/Local/Android/Sdk
```

Build and install over USB or Wi-Fi ADB:

```
cd android
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n io.github.yuk27.vboycolor/android.app.NativeActivity
```

Gradle builds the pinned Google Cardboard SDK v1.34.0 as a library, packages
its QR scanner and Java APIs, and extracts its native Vulkan distortion
renderer for CMake. The first build downloads the SDK's declared Maven
dependencies. See [Cardboard setup and testing](CARDBOARD.md).

The native code is optimized in debug builds too (also when Android Studio's
Run button builds them) - unoptimized, the emulator and the coloring run 2-3x
slower and games stutter on the headset. Configure with
`-DVBGO_OPTIMIZE_DEBUG=OFF` to step through native code in a debugger.

**Signing.** Android installs an APK over an existing one only when both
are signed with the same key. VBoy Color's release key is made once with

```
powershell -ExecutionPolicy Bypass -File tools\make_release_key.ps1
```

which puts `android.keystore` (alias `key0`) and `keystore.txt` (its
password) in a folder outside the repo, points `keystore.dir` in
`android/local.properties` at it (so your `assembleRelease` builds use it),
and adds the repository secrets the CI signs with: `ANDROID_KEYSTORE_BASE64`
and `ANDROID_STORE_PASSWORD` (plus `ANDROID_KEY_PASSWORD` only for a key whose
password differs from the store's). Back that folder up - without it, updates
can't install over the copies people already have. Without a key, branch
builds are signed with a debug key (the CI's changes every run), and a
release tag refuses to build. Each run's summary shows the APK's certificate
SHA-256: the same value on every build means each one installs over the last.
The `versionCode` is the commit count, so each build is also newer than the
one before.

**Color packs:** every build includes the repository's `colorpacks/`.
Your own Quest builds can add a folder of `.vbcp` files with `colorpacks.dir`
in `android/local.properties` (the desktop app's `roms` folder, say, with
forward slashes); those replace the repository's packs of the same name.
Only `.vbcp` files are copied - never ROMs. See
[COLOR_PACKS.md](COLOR_PACKS.md).

**Wi-Fi ADB:** once, on the headset: Settings → Developer → Wireless
debugging → Pair device with pairing code, then `adb pair <ip>:<port>`. Each
session: `adb connect <headset-ip>:5555`.

**Shaders:** after changing `shaders/*.vert|frag`, build a desktop target
first - it regenerates the committed SPIR-V headers in
`core/gfx/generated_shaders/`, which the Android build uses as they are.

## Version string

Settings shows a version generated at build time
(`cmake/GenerateVersion.cmake`): `v<VBGO_VERSION>-dev.<commit count>` (plus
`-dirty` with uncommitted changes), whatever the build type. Only a build
for a numbered release passes `-DVBGO_RELEASE_BUILD=ON` (Android:
`./gradlew assembleRelease -Pofficial=true`), which gives the clean
`v<VBGO_VERSION>`. Bump `VBGO_VERSION` in `CMakeLists.txt` and `versionCode` /
`versionName` in `android/app/build.gradle` for each release.

## Logo and app icon

The logo is `assets/logo/vboycolor-logo.png`. `tools/make_icon.py`
(Pillow) makes everything else from it: the app icon (the logo on a white
tile) for Windows (`platform/desktop/vboycolor.ico`, embedded in both
desktop exes), Linux (`assets/runtime/icon.png`) and the Quest
(`android/app/res/mipmap-*`), and the menu header
(`assets/runtime/logo/vboycolor_header.png`, the logo's two words side by
side). Change the logo, run it, commit the outputs.

## Test harness

`tools/harness/` builds the core plus the coloring code as a shared library
for Python (`ctypes`): load a ROM and save states, run frames, read both
eyes' pixels. The color pack tools and regression checks use it; it never
opens a window.

## RetroArch core

`platform/libretro/` builds VBoy Color as a libretro core, apart from the
app (no Vulkan, OpenXR or window code - only the emulator and the coloring):

```
cmake -S platform/libretro -B build/libretro -DCMAKE_BUILD_TYPE=Release
cmake --build build/libretro --config Release
```

That gives `build/libretro/vboycolor_libretro.so` (`Release\vboycolor_libretro.dll`
on Windows, `.dylib` on macOS). For Android add
`-DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake
-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21`
(`vboycolor_libretro_android.so`); for a universal macOS core,
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`.

How it fits together:

- **Beetle VB** is compiled with its `retro_*` entry points renamed
  `beetle_retro_*` (`platform/libretro/beetle_rename.h`, a forced include),
  with the app's patched VIP (tile tracking, shade tags).
- **`libretro_core.cpp`** is the core the frontend sees. It passes most calls
  through to Beetle VB and colors every frame on the CPU with the app's own
  code (`core/emu`: ShadeColorizer, ColorPackRenderer, TileColorPack;
  the color schemes in `ColorSchemes.h`, shared with the app's settings).
  Then it lays out both eyes for the 3D option. Its core options replace
  Beetle VB's 3D and palette options.
- **The color packs** are zipped into the core at build time
  (`cmake/EmbedColorPacks.cmake`, with an index of the ROMs they were made
  for) and inflated when a game loads.
- **Save states** are Beetle VB's. Loading one forgets the tile tracker's
  tags for any frame buffer that changed a lot. Rewind and run-ahead, which
  load states a frame or two away, keep their colors.
