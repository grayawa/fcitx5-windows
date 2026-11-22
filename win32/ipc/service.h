#pragma once

#include "atomicfile.h"
#include "installation.h"
#include "isolationpolicy.h"
#include "transport.h"
#include <exception>
#include <functional>
#include <string_view>
#include <filesystem>
#include <ntsecapi.h>
#include <stdexcept>
#include <system_error>

namespace fcitx::win32::ipc {

class ServiceHandle {
  public:
    explicit ServiceHandle(HANDLE value = nullptr) : value_(value) {}
    ~ServiceHandle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    ServiceHandle(const ServiceHandle &) = delete;
    ServiceHandle &operator=(const ServiceHandle &) = delete;
    HANDLE get() const { return value_; }

  private:
    HANDLE value_;
};

[[noreturn]] inline void serviceError(DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category());
}

inline std::wstring serviceIdentity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        serviceError();
    }
    ServiceHandle lifetime(token);
    TOKEN_STATISTICS statistics{};
    DWORD size = 0;
    DWORD session = 0;
    const auto sid = userSid();
    if (sid.empty() ||
        !GetTokenInformation(token, TokenStatistics, &statistics,
                             sizeof(statistics), &size) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        serviceError();
    }
    PSECURITY_LOGON_SESSION_DATA logon = nullptr;
    const auto status =
        LsaGetLogonSessionData(&statistics.AuthenticationId, &logon);
    if (status || !logon) {
        serviceError(status ? LsaNtStatusToWinError(status)
                            : ERROR_INVALID_DATA);
    }
    const auto logonTime = logon->LogonTime.QuadPart;
    LsaFreeReturnBuffer(logon);
    auto identity =
        L"Fcitx5Service-" + sid + L"-" + std::to_wstring(session) + L"-" +
        std::to_wstring(statistics.AuthenticationId.HighPart) + L"-" +
        std::to_wstring(statistics.AuthenticationId.LowPart) + L"-" +
        std::to_wstring(logonTime);
#ifdef FCITX5_UI_TEST
    identity += L"-ui-test";
#endif
    return identity;
}

class ServiceSecurity {
  public:
    ServiceSecurity() {
        const auto sid = userSid();
        const auto descriptor = L"D:P(A;;GA;;;" + sid + L")";
        if (sid.empty() ||
            !ConvertStringSecurityDescriptorToSecurityDescriptorW(
                descriptor.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) {
            serviceError();
        }
        attributes_ = {sizeof(attributes_), descriptor_, FALSE};
    }
    ~ServiceSecurity() { LocalFree(descriptor_); }
    SECURITY_ATTRIBUTES *get() { return &attributes_; }

  private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

inline std::filesystem::path serviceStatePath(const std::wstring &identity) {
    std::wstring directory(32768, L'\0');
    const auto size =
        GetEnvironmentVariableW(L"LOCALAPPDATA", directory.data(),
                                static_cast<DWORD>(directory.size()));
    if (!size || size >= directory.size()) {
        serviceError(ERROR_PATH_NOT_FOUND);
    }
    directory.resize(size);
    return std::filesystem::path(directory) / "fcitx5" / (identity + L".state");
}

// Logon time also distinguishes logon LUIDs reused after reboot. The manual
// stop file survives unloading every TSF DLL, but does not affect a new login.
struct ServiceState {
    uint64_t disabled = 0;
    uint64_t lastLaunch = 0;
};

inline ServiceState readServiceState(const std::wstring &identity) {
    ServiceHandle file(
        CreateFileW(serviceStatePath(identity).c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND) {
            return {};
        }
        serviceError();
    }
    ServiceState state;
    DWORD size = 0;
    if (!ReadFile(file.get(), &state, sizeof(state), &size, nullptr) ||
        size != sizeof(state)) {
        serviceError(ERROR_INVALID_DATA);
    }
    return state;
}

inline bool serviceAutoStartAllowed() {
    try {
        return currentInstallationAllowsStart() &&
               !readServiceState(serviceIdentity()).disabled;
    } catch (...) {
        return false;
    }
}

inline std::wstring settingsActivationBase() {
    auto name = pipeName();
    if (name.empty()) {
        serviceError(ERROR_INVALID_DATA);
    }
    for (auto &letter : name) {
        if (letter == L'\\') {
            letter = L'_';
        }
    }
    return L"Local\\Fcitx5Settings-" + name;
}

class ServiceProcess {
  public:
    explicit ServiceProcess(const wchar_t *role,
                            const std::wstring &identity = serviceIdentity())
        : name_(L"Local\\" + identity + L"-" + role),
          owner_(createMutex(name_ + L"-owner")) {}
    ~ServiceProcess() {
        closeStopEvent();
        if (pid_) {
            UnmapViewOfFile(pid_);
        }
        if (mapping_) {
            CloseHandle(mapping_);
        }
        if (owned_) {
            ReleaseMutex(owner_.get());
        }
    }
    bool acquire() {
        const auto result = WaitForSingleObject(owner_.get(), 0);
        if (result == WAIT_TIMEOUT) {
            return false;
        }
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            serviceError();
        }
        owned_ = true;
        ServiceSecurity security;
        stop_ = CreateEventW(security.get(), TRUE, FALSE,
                             (name_ + L"-stop").c_str());
        if (!stop_ || !::ResetEvent(stop_)) {
            serviceError();
        }
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(),
                                      PAGE_READWRITE, 0, sizeof(DWORD),
                                      (name_ + L"-pid").c_str());
        if (!mapping_) {
            serviceError();
        }
        pid_ = static_cast<DWORD *>(
            MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(DWORD)));
        if (!pid_) {
            serviceError();
        }
        *pid_ = GetCurrentProcessId();
        return true;
    }
    bool stopping() const {
        return !currentInstallationAllowsStart() ||
               (stop_ && WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0);
    }
    // Called before the owner mutex is released, after all process work ends.
    void closeStopEvent() {
        if (stop_) {
            CloseHandle(stop_);
            stop_ = nullptr;
        }
    }

  private:
    static HANDLE createMutex(const std::wstring &name) {
        requireHostServices();
        ServiceSecurity security;
        const auto mutex = CreateMutexW(security.get(), FALSE, name.c_str());
        if (!mutex) {
            serviceError();
        }
        return mutex;
    }
    std::wstring name_;
    ServiceHandle owner_;
    HANDLE stop_ = nullptr;
    HANDLE mapping_ = nullptr;
    DWORD *pid_ = nullptr;
    bool owned_ = false;
};

inline bool isServiceProcess(const wchar_t *role) {
    const auto name = L"Local\\" + serviceIdentity() + L"-" + role + L"-pid";
    ServiceHandle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()));
    if (!mapping.get()) {
        return false;
    }
    const auto *pid = static_cast<const DWORD *>(
        MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, sizeof(DWORD)));
    if (!pid) {
        serviceError();
    }
    const auto result = *pid == GetCurrentProcessId();
    UnmapViewOfFile(pid);
    return result;
}

inline void launchServiceExecutable(const std::filesystem::path &path,
                                    bool background,
                                    const wchar_t *arguments = L"") {
    requireHostServices();
    auto command = L"\"" + path.wstring() + L"\"";
    if (*arguments) {
        command += L" ";
        command += arguments;
    }
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
                        background ? CREATE_NO_WINDOW : 0, nullptr,
                        path.parent_path().c_str(), &startup, &process)) {
        serviceError();
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

class ServiceController {
  public:
    using Logger = std::function<void(std::string_view)>;

    explicit ServiceController(std::filesystem::path core,
                               std::wstring identity = serviceIdentity(),
                               Logger logger = {})
        : core_(std::move(core)), identity_(std::move(identity)),
          logger_(std::move(logger)), control_(createControlMutex()) {
        log("control mutex wait begin timeout_ms=5000");
        const auto started = GetTickCount64();
        const auto result = WaitForSingleObject(control_.get(), 5000);
        const auto elapsed = GetTickCount64() - started;
        log("control mutex wait result=" + std::to_string(result) +
            " elapsed_ms=" + std::to_string(elapsed));
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            const auto error = result == WAIT_TIMEOUT ? ERROR_TIMEOUT
                                                      : GetLastError();
            log("control mutex acquisition failed error=" +
                std::to_string(error));
            serviceError(error);
        }
        locked_ = true;
        log(result == WAIT_ABANDONED ? "control mutex acquired abandoned"
                                     : "control mutex acquired");
    }
    ~ServiceController() {
        if (locked_) {
            ReleaseMutex(control_.get());
        }
    }
    bool ensureCore(bool explicitStart = false) {
        log("ensureCore begin explicit=" + std::to_string(explicitStart));
        if (!InstallationGate(core_.parent_path().parent_path())
                 .allowsStart()) {
            log("ensureCore blocked by installation gate");
            return false;
        }
        auto state = readServiceState(identity_);
        if (state.disabled) {
            log("ensureCore blocked disabled=1");
            return false;
        }
        if (running(L"Core")) {
            log("ensureCore Core already running");
            return true;
        }
        const auto now = GetTickCount64();
        if (!explicitStart && state.lastLaunch && now >= state.lastLaunch &&
            now - state.lastLaunch < 10000) {
            log("ensureCore launch throttled");
            return false;
        }
        state.lastLaunch = now;
        save(state);
        log("ensureCore CreateProcess begin path=" +
            core_.u8string().empty()
                ? std::string()
                : std::string(core_.u8string().begin(), core_.u8string().end()));
        launchServiceExecutable(core_, true);
        log("ensureCore CreateProcess returned; waiting for Core owner");
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            if (running(L"Core")) {
                log("ensureCore Core owner acquired after_ms=" +
                    std::to_string((attempt + 1) * 20));
                return true;
            }
            Sleep(20);
        }
        log("ensureCore Core owner wait timed out");
        serviceError(ERROR_SERVICE_REQUEST_TIMEOUT);
        return false;
    }
    void stop() {
        log("stop begin");
        auto state = readServiceState(identity_);
        state.disabled = 1;
        save(state);
        log("stop persisted disabled=1");
        std::exception_ptr failure;
        for (const auto role : {L"Settings", L"Core"}) {
            try {
                stopProcess(role);
            } catch (const std::system_error &error) {
                log("stop role failed role=" + roleName(role) +
                    " error=" + std::to_string(error.code().value()));
                if (!failure) {
                    failure = std::current_exception();
                }
            } catch (...) {
                log("stop role failed role=" + roleName(role) +
                    " error=unknown");
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        }
        if (failure) {
            log("stop failed");
            std::rethrow_exception(failure);
        }
        log("stop complete");
    }
    bool restart() {
        const auto previous = readServiceState(identity_);
        log("restart begin previous_disabled=" +
            std::to_string(previous.disabled) + " previous_lastLaunch=" +
            std::to_string(previous.lastLaunch));
        try {
            const auto settingsOpen = running(L"Settings");
            log(std::string("restart Settings running=") +
                (settingsOpen ? "1" : "0"));
            stop();
            auto state = readServiceState(identity_);
            state.disabled = 0;
            state.lastLaunch = 0;
            save(state);
            log("restart persisted disabled=0; ensuring Core");
            ensureCore(true);
            log("restart complete");
            return settingsOpen;
        } catch (...) {
            const auto failure = std::current_exception();
            try {
                auto state = readServiceState(identity_);
                state.disabled = previous.disabled;
                state.lastLaunch = previous.lastLaunch;
                save(state);
                log("restart failure restored previous service state");
            } catch (const std::system_error &error) {
                log("restart failed to restore service state error=" +
                    std::to_string(error.code().value()));
            } catch (...) {
                log("restart failed to restore service state error=unknown");
            }
            std::rethrow_exception(failure);
        }
    }
    bool running(const wchar_t *role) const {
        ServiceHandle owner(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                       objectName(role, L"-owner").c_str()));
        if (!owner.get()) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) {
                serviceError();
            }
            return false;
        }
        const auto wait = WaitForSingleObject(owner.get(), 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            ReleaseMutex(owner.get());
            return false;
        }
        if (wait != WAIT_TIMEOUT) {
            serviceError();
        }
        return true;
    }

  private:
    std::wstring objectName(const wchar_t *role, const wchar_t *suffix) const {
        return L"Local\\" + identity_ + L"-" + role + suffix;
    }
    HANDLE createControlMutex() {
        requireHostServices();
        ServiceSecurity security;
        const auto mutex =
            CreateMutexW(security.get(), FALSE,
                         (L"Local\\" + identity_ + L"-control").c_str());
        if (!mutex) {
            serviceError();
        }
        return mutex;
    }
    void save(const ServiceState &state) const {
        const auto path = serviceStatePath(identity_);
        if (!replaceFile(
                path, std::string_view(reinterpret_cast<const char *>(&state),
                                       sizeof(state)))) {
            serviceError();
        }
    }
    void stopProcess(const wchar_t *role) const {
        const auto name = roleName(role);
        log("stopProcess begin role=" + name);
        if (!running(role)) {
            log("stopProcess role not running role=" + name);
            return;
        }
        ServiceHandle owner(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                       objectName(role, L"-owner").c_str()));
        if (!owner.get()) {
            const auto error = GetLastError();
            log("stopProcess open owner failed role=" + name +
                " error=" + std::to_string(error));
            serviceError(error);
        }
        const auto end = GetTickCount64() + 3000;
        unsigned signals = 0;
        do {
            // Acquisition publishes ownership just before creating/resetting
            // the event. Re-signal while waiting so startup cannot lose a stop.
            ServiceHandle event(OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                           objectName(role, L"-stop").c_str()));
            if (event.get()) {
                if (!SetEvent(event.get())) {
                    const auto error = GetLastError();
                    log("stopProcess SetEvent failed role=" + name +
                        " error=" + std::to_string(error));
                    serviceError(error);
                }
                ++signals;
            } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
                const auto error = GetLastError();
                log("stopProcess open stop event failed role=" + name +
                    " error=" + std::to_string(error));
                serviceError(error);
            }
            const auto wait = WaitForSingleObject(owner.get(), 20);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
                ReleaseMutex(owner.get());
                log("stopProcess owner released role=" + name +
                    " stop_signals=" + std::to_string(signals));
                return;
            }
            if (wait != WAIT_TIMEOUT) {
                const auto error = GetLastError();
                log("stopProcess owner wait failed role=" + name +
                    " wait=" + std::to_string(wait) +
                    " error=" + std::to_string(error));
                serviceError(error);
            }
        } while (GetTickCount64() < end);
        log("stopProcess timed out role=" + name +
            " stop_signals=" + std::to_string(signals) +
            " timeout_ms=3000");
        serviceError(ERROR_TIMEOUT);
    }
    static std::string roleName(const wchar_t *role) {
        return role && wcscmp(role, L"Settings") == 0 ? "Settings" : "Core";
    }
    void log(std::string message) const noexcept {
        if (logger_) {
            try {
                logger_(message);
            } catch (...) {
            }
        }
    }
    std::filesystem::path core_;
    std::wstring identity_;
    Logger logger_;
    ServiceHandle control_;
    bool locked_ = false;
};

} // namespace fcitx::win32::ipc
