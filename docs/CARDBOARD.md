# Google Cardboard on Android

## Enable and configure

1. Open VBoy Color on an Android phone. A clean install starts in Google
   Cardboard mode; existing saved display choices are preserved. To change
   the mode, open **Settings → Screen → 3D** and select **Google Cardboard**.
2. Choose **Scan viewer QR code** and scan the code supplied with the viewer.
   Google's scanner accepts standard Cardboard viewer profiles, including
   custom-generated profiles. Camera access is requested by the SDK scanner.
3. Start a game and place the phone in the viewer. In **Settings → Screen**,
   use **Cardboard screen size** to adjust the image from 50% to 125% in 5%
   steps. It starts at 80%; lower values make the centered image smaller and
   appear farther away, while higher values make it larger and appear closer.
   **Pixel scaling** still chooses **Fit** or **Whole pixels** for the source
   pixels.

If no profile has been saved, Cardboard's SDK-provided V1 profile is used as a
fallback. Scanning the actual viewer's QR profile is recommended for accurate
lens placement and distortion. Scan again later to change viewers. Canceling
the scanner or denying camera access keeps the current profile and returns to
the app.

## Controls and exit

- Select + Start or the gamepad's Guide button opens the existing menu.
- Use the mapped gamepad D-pad/stick and confirm button to navigate. The menu
  is rendered as the same centered plane in both eyes and receives the same
  Cardboard optical correction as the game.
- With the phone outside the viewer, tap the menu in the left-eye half of the
  display to choose a game or setting. In the viewer, use a Bluetooth gamepad.
- **Android Back exits Cardboard mode immediately**, even when the menu is
  open. The normal phone display returns without requiring a controller.
- Touch gameplay controls and the ordinary phone overlays are hidden while
  Cardboard is selected. Bluetooth gamepads and the Virtual Boy mappings work
  as before.

The app remains landscape, as it does for the existing phone renderer. Pausing
and resuming the activity, changing landscape orientation, resizing the
surface, or returning from the QR scanner causes the affected swapchain or
viewer meshes to be rebuilt.

## Rendering notes

The dependency is the official [Google Cardboard SDK v1.34.0](https://github.com/googlevr/cardboard/releases/tag/v1.34.0), pinned as the
`third_party/cardboard` Git submodule. Gradle builds its official Android
library and extracts `libGfxPluginCardboard.so` plus `cardboard.h` for the
Android CMake target. The app uses Cardboard's saved-device-parameters APIs,
the V1 fallback profile, the QR activity, per-eye lens distortion meshes and
the Vulkan distortion renderer.

The phone renderer calls the existing `Emulator::PrepareScreen` once for the
emulated frame, then draws its colorized left and right eye crops into one
reusable RGBA sRGB atlas. Each 384×224 eye image is fitted independently into
its half of the atlas, preserving the Virtual Boy's aspect ratio and encoded
stereo disparity. The Cardboard SDK samples that atlas in `GENERAL` layout and
distorts the two eyes into a depth-backed Vulkan display pass. This keeps the
existing palette, color pack, Sharp/Smooth/LED processing and avoids an extra
emulator run, CPU readback, or per-frame image allocation.

Cardboard screen size scales the centered source image inside each eye viewport
without changing its aspect ratio or separation. A smaller angular image gives
the impression of a farther screen; this does not add a virtual scene or head
tracking. The SDK lens profile remains responsible for optical correction and
eye geometry.

Cardboard is deliberately zero-DOF: VBoy Color never creates a Cardboard head
tracker or reads gyroscope/accelerometer pose. A profile change rebuilds the
SDK lens object and distortion meshes in place; surface recreation rebuilds
the swapchain-bound distortion renderer and depth targets before old Vulkan
resources are released. If setup fails, the setting resets to the normal phone
display and a toast explains the fallback.

## Manual test: Samsung Galaxy S21+ and a generic viewer

1. Build `android/app/build/outputs/apk/debug/app-debug.apk`, install it on the
   Galaxy S21+, and launch VBoy Color normally.
2. Choose the ROMs folder if prompted. The library should show the ROMs in that
   folder; tap Wario Land in the left-eye half to start it before putting the
   phone in the viewer. A clean install already starts in Cardboard mode; on an
   existing install, select Settings → Screen → 3D → Google Cardboard.
3. Select **Scan viewer QR code**, grant camera permission, and scan the QR
   code printed for the generic viewer. For a custom viewer, generate a
   standard Google Cardboard profile QR code and scan that instead.
4. Connect a Bluetooth gamepad. Put the phone in the viewer and verify that
   the left game image is seen by the left eye and the
   right image by the right eye. Check that the display is landscape, the
   picture is not stretched or mirrored, and color palettes and display looks
   remain active.
5. Adjust Cardboard screen size from 50% to 125% and confirm both images stay
   centered and keep their aspect ratio. Compare Fit and Whole pixels under
   Pixel scaling. Open the menu with Guide or Select + Start and verify that
   both eyes show the same corrected menu plane.
6. Press Android Back while playing and confirm the normal phone display
   returns. Also test Android pause/resume, rotating between landscape
   orientations, rescanning a different profile, and canceling a scan.
7. Re-enter Cardboard mode after the rescan and confirm the new profile takes
   effect without restarting the app.

Physical Galaxy/viewer behavior still needs on-device validation. A successful
APK build verifies compilation and packaging, not optical fit, camera behavior,
or headset comfort.
