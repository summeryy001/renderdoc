# Custom Build Notes

This document records build-related settings discovered from the old SVN customized RenderDoc trees.

The current migration policy is:

- Prefer CMake cache options over hard-coded source edits.
- Do not migrate machine-specific SDK paths or fixed local environment assumptions into source.
- Keep old SVN values documented so old builds can be reproduced when needed.

## Old SVN Build Baseline

The old SVN customization had a build-related change in:

- `dev/renderdoc-1.x` revision `r36`
- merged into `dev/renderdoc-1.x_20251113` revision `r60`

Changed areas:

- root `CMakeLists.txt`
- `renderdoc/replay/version.cpp`
- `renderdoccmd/CMakeLists.txt`

Old behavior:

- Forced `STRIP_ANDROID_LIBRARY` to `ON`.
- Hard-coded `GIT_COMMIT_HASH`.
- Forced Android build-tools to `26.0.1`.
- Forced Android APK target platform to `android-23`.

Old hard-coded hashes:

- `renderdoc-1.x`: `742886b1b6496662eddbbdf5e581896d42607982`
- `renderdoc-1.x_20251113`: `edf9ddacf0608e49293c2cf0a33cd8d9c43f17b3`

## Current Git Policy

Do not hard-code those values into source unless there is a confirmed reason.

Use CMake options instead:

```bash
-DSTRIP_ANDROID_LIBRARY=ON
-DBUILD_VERSION_HASH=<git-commit-hash>
-DANDROID_BUILD_TOOLS_VERSION=26.0.1
-DAPK_TARGET_ID=android-23
```

For current upstream builds, prefer the installed latest Android build-tools/platform unless old APK patch compatibility requires the exact legacy versions.

## Suggested Windows Desktop Validation

For non-Android changes:

1. Configure/build with Visual Studio.
2. Build `x64` `Development`.
3. Launch `qrenderdoc.exe`.
4. Confirm the UI starts and basic capture loading still works.

## Suggested Android Validation

For Android migration steps:

1. Confirm Android SDK / NDK / JDK environment.
2. Configure Android build using explicit CMake options only if needed.
3. Build `renderdoccmd.apk`.
4. Verify `adb devices`.
5. Install or patch a test APK.
6. Confirm target app can be captured.

## Legacy Reproduction Example

If a test requires reproducing old SVN Android build assumptions:

```bash
cmake ^
  -DSTRIP_ANDROID_LIBRARY=ON ^
  -DBUILD_VERSION_HASH=edf9ddacf0608e49293c2cf0a33cd8d9c43f17b3 ^
  -DANDROID_BUILD_TOOLS_VERSION=26.0.1 ^
  -DAPK_TARGET_ID=android-23 ^
  <other-options>
```

This should be treated as legacy compatibility mode, not the default migration target.

