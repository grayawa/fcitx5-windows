#pragma once

namespace fcitx::setup {

#if defined(_M_ARM64) || defined(__aarch64__)
inline constexpr char kNativeArchitecture[] = "arm64";
inline constexpr char kNativeTsfDll[] = "tsf/fcitx5-arm64.dll";
inline constexpr wchar_t kNativeTsfDllWide[] = L"tsf/fcitx5-arm64.dll";
inline constexpr char kVCRedistFile[] = "vc_redist.arm64.exe";
inline constexpr char kWindowsAppRuntimeFile[] =
    "WindowsAppRuntimeInstall-arm64.exe";
inline constexpr wchar_t kWindowsAppRuntimeFileWide[] =
    L"WindowsAppRuntimeInstall-arm64.exe";
#else
inline constexpr char kNativeArchitecture[] = "x64";
inline constexpr char kNativeTsfDll[] = "tsf/fcitx5-x86_64.dll";
inline constexpr wchar_t kNativeTsfDllWide[] = L"tsf/fcitx5-x86_64.dll";
inline constexpr char kVCRedistFile[] = "vc_redist.x64.exe";
inline constexpr char kWindowsAppRuntimeFile[] =
    "WindowsAppRuntimeInstall-x64.exe";
inline constexpr wchar_t kWindowsAppRuntimeFileWide[] =
    L"WindowsAppRuntimeInstall-x64.exe";
#endif

} // namespace fcitx::setup
