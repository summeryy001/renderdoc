# RenderDoc Custom Migration Plan

Owner fork: `summeryy001/renderdoc`  
Active branch: `custom`  
Clean upstream branch: `v1.x`  
Migration base tag: `custom-migration-base-20260701`

## Goal

把旧 SVN 定制版中的稳定修改，按 feature 小步迁移到新的 Git `custom` 分支中。

原则：

- SVN 仓库只作为 historical reference，不继续作为主开发仓库。
- Git 仓库作为 active development workspace。
- 每一小步只迁移一个清晰 feature。
- 每步迁移后先本地编译/运行验证。
- 只有用户确认 OK 后才 commit。
- 阶段稳定后再 push 到 `origin/custom`。

## Repository Roles

| Remote / Branch | Meaning |
| --- | --- |
| `upstream/v1.x` | 官方 RenderDoc 上游。 |
| `origin/v1.x` | `summeryy001` fork 中保持干净同步的官方线。 |
| `custom` | 本地定制维护分支。 |
| `origin/custom` | GitHub 上的定制维护分支。 |
| `custom-migration-base-20260701` | 开始迁移旧 SVN 定制前的干净基线。 |

## Migration Workflow

每个 feature 固定流程：

1. Select a small feature from old SVN history.
2. Compare SVN customized source with new Git upstream source.
3. Port only the minimal required code into Git `custom`.
4. Build locally.
5. Run focused validation.
6. User confirms result is OK.
7. Review `git diff`.
8. Commit with a focused message.
9. Push only after a stable group of commits is ready.

## Commit Rule

不要做一次性大搬迁。

Good commit style:

```text
Android: add APK debuggable manifest patching
Android: adjust target control port handling
GL: support EGLImage target texture entrypoints
UI: restore custom texture export actions
Build: restore Android SDK/strip configuration
```

Bad commit style:

```text
merge old svn changes
custom changes
copy 1.x_20251113
```

## Validation Policy

### Common Validation

每次迁移后至少检查：

- `git status --short --branch`
- `git diff`
- changed file list 是否只包含本次 feature 相关文件

### Visual Studio Validation

Windows 桌面端改动优先用 Visual Studio 验证：

- 生成或打开 VS solution。
- 编译 `x64`。
- 优先验证 `Development` 配置，必要时再验证 `Release`。
- 启动 `qrenderdoc.exe`。
- 确认 UI 能打开，基本操作无异常。

适用 feature：

- TextureViewer / TextureSaveDialog
- LiveCapture / CaptureDialog
- PC / Steam capture fix
- Windows build/project changes

### Android Validation

Android 相关改动除 VS 编译外，还需要按实际场景验证：

- Android SDK / NDK path。
- JDK / Java compatibility，历史定制里有 Java 8 相关修改。
- `renderdoccmd.apk` build/install。
- `adb devices` 能识别设备。
- APK patch / reinstall 流程。
- target app 能进入 debuggable/capture 状态。
- RenderDoc 能连接 remote server / target control。

适用 feature：

- APK debuggable patch
- binary manifest patch
- APK zip/unzip repack
- Android process/package selection
- Android socket / target control / remote server

### GL / EGL Validation

GL/EGL external texture 相关改动需要专项验证：

- Android GLES app capture。
- 使用 EGLImage / external texture 的场景。
- Capture 能成功。
- Replay 中纹理显示正常。
- 不影响普通 GL texture capture。

适用 feature：

- `glEGLImageTargetTexture2DOES`
- `glEGLImageTargetRenderbufferStorageOES`
- `glEGLImageTargetTexStorageEXT`
- `glEGLImageTargetTextureStorageEXT`

## Planned Migration Order

### 0. Documentation and Git Setup

Status: in progress

Scope:

- Create Git fork workflow.
- Add `upstream`.
- Create `custom` branch.
- Add migration base tag.
- Add this migration plan.

Validation:

- `origin`, `upstream` remotes exist.
- `custom` tracks `origin/custom`.
- base tag exists both locally and on origin.

### 1. Build / Version / Android SDK Baseline

Status: pending

Source SVN hints:

- `dev/renderdoc-1.x` r36
- merged into `dev/renderdoc-1.x_20251113` r60

Likely files:

- `CMakeLists.txt`
- `renderdoc/replay/version.cpp`
- `renderdoccmd/CMakeLists.txt`

Purpose:

- Restore custom build flags and Android SDK handling.
- Restore `STRIP_ANDROID_LIBRARY` related behavior if still needed.

Validation:

- Configure CMake / VS project.
- Build x64 Development.
- If Android build is touched, also verify Android configure step.

### 2. Android APK Debuggable Patch

Status: pending

Source SVN hints:

- `dev/renderdoc-1.x` r37
- merged into `dev/renderdoc-1.x_20251113` r61

Likely files:

- `renderdoc/3rdparty/aosp/android_manifest.h`
- `renderdoc/android/android_manifest.cpp`
- `renderdoc/android/android_patch.cpp`
- `renderdoc/android/android.cpp`
- `renderdoc/android/android_utils.cpp`
- `renderdoc/android/android_utils.h`
- `renderdoc/api/replay/renderdoc_replay.h`
- `renderdoc/api/replay/replay_enums.h`
- `qrenderdoc/Windows/Dialogs/CaptureDialog.cpp`

Purpose:

- Restore APK binary manifest patching for `debuggable`.
- Restore UI/API plumbing needed for patch flow.

Validation:

- Build x64 Development.
- Launch UI.
- Patch a test APK.
- Install/reinstall target APK.
- Confirm target app becomes debuggable/capturable.

### 3. Android Target Control / Remote Server

Status: pending

Source SVN hints:

- `dev/renderdoc-1.22` r5-r18
- carried into `dev/renderdoc-1.x` r37
- merged into `dev/renderdoc-1.x_20251113` r61

Likely files:

- `renderdoc/android/android.cpp`
- `renderdoc/android/android.h`
- `renderdoc/android/android_utils.cpp`
- `renderdoc/core/remote_server.cpp`
- `renderdoc/core/remote_server.h`
- `renderdoc/core/target_control.cpp`
- `renderdoc/os/os_specific.h`

Purpose:

- Restore Android socket / port / process launch behavior.
- Restore custom target control communication path.

Validation:

- Build x64 Development.
- Connect Android device.
- Launch remote server.
- Capture a test Android app.

### 4. APK Repack with Zip/Unzip and Java 8 Compatibility

Status: pending

Source SVN hints:

- `dev/renderdoc-1.x` r49, r51
- merged into `dev/renderdoc-1.x_20251113` r65, r66

Likely files:

- `renderdoc/android/android_patch.cpp`
- `renderdoc/android/android_tools.cpp`
- `renderdoc/android/android_utils.h`

Purpose:

- Restore APK repack flow that avoids fragile `aapt`/Gradle dependency.
- Restore Java 8 compatibility changes.

Validation:

- Build x64 Development.
- Patch/repack APK.
- Reinstall APK.
- Verify no Java version failure in the patch flow.

### 5. GL / EGL External Texture Support

Status: pending

Source SVN hints:

- `dev/renderdoc-1.x` r46
- merged into `dev/renderdoc-1.x_20251113` r64

Likely files:

- `CMakeLists.txt`
- `renderdoc/driver/gl/gl_common.h`
- `renderdoc/driver/gl/gl_dispatch_table.h`
- `renderdoc/driver/gl/gl_dispatch_table_defs.h`
- `renderdoc/driver/gl/gl_driver.cpp`
- `renderdoc/driver/gl/gl_driver.h`
- `renderdoc/driver/gl/gl_initstate.cpp`
- `renderdoc/driver/gl/gl_manager.cpp`
- `renderdoc/driver/gl/wrappers/gl_texture_funcs.cpp`

Purpose:

- Restore support for EGLImage related entrypoints:
  - `glEGLImageTargetTexture2DOES`
  - `glEGLImageTargetRenderbufferStorageOES`
  - `glEGLImageTargetTexStorageEXT`
  - `glEGLImageTargetTextureStorageEXT`

Validation:

- Build x64 Development.
- Android GLES capture using external texture/EGLImage.
- Replay and inspect texture output.

### 6. TextureViewer Save / Export Enhancements

Status: pending

Source SVN hints:

- `dev/renderdoc-1.22` r23-r31, r38
- `dev/renderdoc-1.x` r39, r41
- merged into `dev/renderdoc-1.x_20251113` r62, r63

Likely files:

- `qrenderdoc/Windows/TextureViewer.cpp`
- `qrenderdoc/Windows/TextureViewer.h`
- `qrenderdoc/Windows/TextureViewer.ui`
- `qrenderdoc/Windows/Dialogs/TextureSaveDialog.cpp`
- `qrenderdoc/Windows/Dialogs/TextureSaveDialog.h`
- `qrenderdoc/Code/Resources.h`
- `qrenderdoc/Resources/resources.qrc`
- `qrenderdoc/Resources/save2.png`
- `qrenderdoc/Resources/save2@2x.png`
- `qrenderdoc/Resources/save3.png`
- `qrenderdoc/Resources/save3@2x.png`

Purpose:

- Restore custom texture save/export UI actions and resources.

Validation:

- Build x64 Development.
- Launch `qrenderdoc.exe`.
- Open a capture.
- Verify TextureViewer custom save/export actions.

### 7. PC / Steam Capture Fix

Status: pending

Source SVN hints:

- `dev/renderdoc-1.22` r33

Likely files:

- `qrenderdoc/Windows/Dialogs/LiveCapture.cpp`

Purpose:

- Restore PC/Steam capture related bugfix if still relevant.

Validation:

- Build x64 Development.
- Run target desktop app / Steam app scenario.
- Verify capture flow.

## Open Questions

- Which Visual Studio version should be the standard validation environment?
- Which CMake generator/config should be considered official for this fork?
- Which Android SDK/NDK/JDK versions are currently installed on the build PC?
- Which APK/app should be the standard Android smoke test target?
- Which capture file should be used for TextureViewer save/export smoke testing?

