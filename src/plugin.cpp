#include <D2RLPlugin/api.h>
#include <D2RLPlugin/logging.h>
#include <D2RLPlugin/panels.h>
#include <D2RLPlugin/resources.h>
#include <D2RLPlugin/shared_events.h>

#include <Windows.h>
#include <Shlwapi.h>
#include <Shellapi.h>

#include <array>
#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "default_config.hpp"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

constexpr std::uint16_t BrowserPanelLayoutResourceId = 2001;
constexpr std::uint16_t OpenExternalButtonResourceId = 2002;
constexpr std::uint16_t OpenExternalButtonLowEndResourceId = 2003;
constexpr char PluginId[] = "d2rl-ingame-webbrowser";
constexpr char BrowserPanelLocalId[] = "BrowserPanel";
constexpr char BrowserPanelName[] = "d2rl-ingame-webbrowser/BrowserPanel";
constexpr std::size_t MaximumConfigBytes = 16U * 1024U;
constexpr std::size_t MaximumBrowserPanelLayoutBytes = 4U * 1024U * 1024U;
constexpr wchar_t BrowserPanelLayoutFileName[] = L"BrowserPanelhd.json";
constexpr char BrowserPanelLayoutPath[] =
    "data/global/ui/layouts/d2rl-ingame-webbrowser/BrowserPanelhd.json";
constexpr char OpenExternalButtonResourcePath[] =
    "data/hd/global/ui/d2rloader/d2rl-ingame-webbrowser/open-external-button.sprite";
constexpr char OpenExternalButtonLowEndResourcePath[] =
    "data/hd/global/ui/d2rloader/d2rl-ingame-webbrowser/open-external-button.lowend.sprite";

constexpr char ClickMessageTarget[] = "PanelManager";
constexpr char TogglePanelCommand[] = "TogglePanel";
constexpr char ClosePanelCommand[] = "ClosePanel";
constexpr char OpenExternalPageAction[] = "OpenExternalPage";
constexpr wchar_t EscapeMessageWindowClass[] =
    L"MadMike.WebbrowserInGame.EscapeMessageWindow";
constexpr UINT_PTR BrowserCloseRetryTimerId = 0x4B3;
constexpr UINT BrowserReadyMessage = WM_APP + 0x4B4;
constexpr UINT CefCloseBrowserMessage = WM_APP + 0x4B5;
constexpr UINT NativePanelOpenMessage = WM_APP + 0x4B7;
constexpr UINT NativePanelCloseMessage = WM_APP + 0x4B8;
constexpr UINT NativePanelOpenExternalMessage = WM_APP + 0x4B9;

const D2RL::PluginContext* Context{};
const D2RL::ResourceService* ResourceService{};
const D2RL::PanelService* PanelService{};
const D2RL::SharedEventService* SharedEventService{};
D2RL::Resources::RegistrationHandle BrowserPanelLayoutResource{
    D2RL::Resources::InvalidHandle};
D2RL::Resources::RegistrationHandle OpenExternalButtonResource{
    D2RL::Resources::InvalidHandle};
D2RL::Resources::RegistrationHandle OpenExternalButtonLowEndResource{
    D2RL::Resources::InvalidHandle};
D2RL::Panels::RegistrationHandle BrowserPanel{
    D2RL::Panels::InvalidHandle};
D2RL::SharedEvents::ListenerHandle UiMessageListener{
    D2RL::SharedEvents::InvalidHandle};
HWND GameWindow{};
HANDLE BrowserProcess{};
DWORD BrowserProcessId{};
std::uint64_t BrowserSessionToken{};
DWORD BrowserSessionProcessId{};
HWND BrowserControlWindow{};
bool BrowserCloseRequested{};
bool BrowserCloseMessageSent{};
bool ClosePanelAfterBrowserExit{};
bool BrowserReady{};
bool BrowserPanelOpen{};
HWND EscapeMessageWindow{};
DWORD EscapeMessageWindowThreadId{};
HHOOK BrowserEscapeHook{};
std::atomic_bool BrowserEscapeActive{};
std::atomic_bool BrowserEscapeCloseQueued{};
std::string ConfiguredHtmlFile;

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .abiVersion = D2RL_PLUGIN_ABI_VERSION,
    .id = PluginId,
    .name = "Webbrowser In-Game Plugin",
    .version = "0.2.36",
    .author = "MadMike",
    .description = "Opens a configured local HTML page in-game or in the default browser.",
    .flags = D2RL::PluginFlags::Client,
};

bool IsFile(const std::wstring& path) noexcept {
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string_view Trim(std::string_view text) noexcept {
    while (!text.empty()
        && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty()
        && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view StripComment(std::string_view text) noexcept {
    char quote{};
    bool escaped{};
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quote == '"' && ch == '\\') {
            escaped = true;
            continue;
        }
        if (quote == '\0' && (ch == '"' || ch == '\'')) {
            quote = ch;
        } else if (quote != '\0' && ch == quote) {
            quote = '\0';
        } else if (quote == '\0' && ch == '#') {
            return text.substr(0, i);
        }
    }
    return text;
}

bool ParseTomlString(std::string_view value, std::string& output) {
    value = Trim(StripComment(value));
    if (value.size() < 2
        || (value.front() != '"' && value.front() != '\'')
        || value.back() != value.front()) {
        return false;
    }

    output.clear();
    if (value.front() == '\'') {
        output.assign(value.substr(1, value.size() - 2));
        return true;
    }

    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '\\') {
            if (i + 1 >= value.size() - 1) return false;
            const char escaped = value[++i];
            switch (escaped) {
            case '\\': output.push_back('\\'); break;
            case '"': output.push_back('"'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            default: return false;
            }
        } else {
            output.push_back(ch);
        }
    }
    return true;
}

bool FindTomlString(
        std::string_view config,
        std::string_view key,
        std::string& output) {
    std::size_t start{};
    while (start < config.size()) {
        const auto end = config.find('\n', start);
        const auto line = Trim(StripComment(config.substr(
            start, end == std::string_view::npos
                ? config.size() - start : end - start)));
        const auto equals = line.find('=');
        if (equals != std::string_view::npos
            && Trim(line.substr(0, equals)) == key) {
            return ParseTomlString(line.substr(equals + 1), output);
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return false;
}

bool ReadConfiguration() noexcept {
    if (!Context || !Context->EnsureConfig(DefaultConfig)) {
        if (Context) {
            Context->LogError(
                "WebbrowserInGame: could not create or access the plugin TOML config.");
        }
        return false;
    }

    try {
        std::array<char, MaximumConfigBytes> buffer{};
        std::uint32_t requiredSize{};
        if (!Context->ReadConfig(
                buffer.data(), static_cast<std::uint32_t>(buffer.size()),
                &requiredSize)
            || requiredSize > buffer.size()) {
            Context->LogError(
                "WebbrowserInGame: could not read the TOML config or it exceeds 16 KiB.");
            return false;
        }

        std::string configuredPath;
        if (!FindTomlString(
                std::string_view(buffer.data()), "html_file", configuredPath)
            || Trim(configuredPath).empty()) {
            Context->LogError(
                "WebbrowserInGame: html_file must be a non-empty quoted TOML string.");
            return false;
        }
        ConfiguredHtmlFile = std::move(configuredPath);
        return true;
    } catch (...) {
        Context->LogError(
            "WebbrowserInGame: an error occurred while reading the TOML config.");
        return false;
    }
}

bool Utf8ToWide(std::string_view input, std::wstring& output) {
    if (input.empty() || input.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    const auto inputSize = static_cast<int>(input.size());
    const auto required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), inputSize, nullptr, 0);
    if (required <= 0) return false;
    output.resize(static_cast<std::size_t>(required));
    return MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), inputSize,
        output.data(), required) == required;
}

bool ResolvePluginDirectory(std::wstring& directory) {
    if (Context) {
        const auto* configured = D2RL::GetPluginDirectory(Context);
        if (configured && configured[0] != L'\0') {
            directory = configured;
            return true;
        }
    }

    std::vector<wchar_t> path(32768);
    const auto count = GetModuleFileNameW(
        reinterpret_cast<HMODULE>(&__ImageBase),
        path.data(),
        static_cast<DWORD>(path.size()));
    if (count == 0 || count >= path.size()) return false;
    directory.assign(path.data(), count);
    const auto slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    directory.resize(slash);
    return true;
}

bool LoadEmbeddedLayout(
        std::uint16_t resourceId,
        std::vector<std::uint8_t>& bytes) noexcept {
    const auto module = reinterpret_cast<HMODULE>(&__ImageBase);
    const auto resource = FindResourceW(
        module,
        MAKEINTRESOURCEW(resourceId),
        RT_RCDATA);
    if (!resource) return false;

    const auto size = SizeofResource(module, resource);
    const auto loaded = LoadResource(module, resource);
    const auto* data = loaded
        ? static_cast<const std::uint8_t*>(LockResource(loaded))
        : nullptr;
    if (!data || size == 0) return false;

    try {
        bytes.assign(data, data + size);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadBrowserPanelLayoutFile(
        const std::wstring& path,
        std::vector<std::uint8_t>& bytes) noexcept {
    const auto file = CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize)
        || fileSize.QuadPart <= 0
        || static_cast<unsigned long long>(fileSize.QuadPart)
            > MaximumBrowserPanelLayoutBytes) {
        CloseHandle(file);
        return false;
    }

    try {
        bytes.resize(static_cast<std::size_t>(fileSize.QuadPart));
    } catch (...) {
        CloseHandle(file);
        return false;
    }

    DWORD bytesRead{};
    const auto read = ReadFile(
        file, bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, nullptr);
    CloseHandle(file);
    return read != FALSE && static_cast<std::size_t>(bytesRead) == bytes.size();
}

bool LoadBrowserPanelLayoutFile(std::vector<std::uint8_t>& bytes) noexcept {
    try {
        std::wstring pluginDirectory;
        if (!ResolvePluginDirectory(pluginDirectory)) {
            Context->LogError(
                "WebbrowserInGame: plugin directory could not be resolved for BrowserPanelhd.json.");
            return false;
        }

        const auto pluginPath = std::filesystem::path(pluginDirectory);
        const auto layoutPath = (pluginPath / BrowserPanelLayoutFileName).wstring();
        if (!IsFile(layoutPath)) {
            std::vector<std::uint8_t> defaultBytes;
            if (!LoadEmbeddedLayout(BrowserPanelLayoutResourceId, defaultBytes)
                || defaultBytes.empty()
                || defaultBytes.size() > MaximumBrowserPanelLayoutBytes) {
                Context->LogError(
                    "WebbrowserInGame: could not load the built-in BrowserPanelhd.json default.");
                return false;
            }

            const auto file = CreateFileW(
                layoutPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                const auto error = GetLastError();
                if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                    Context->LogError(
                        "WebbrowserInGame: could not create BrowserPanelhd.json beside the plugin DLL.");
                    return false;
                }
            } else {
                DWORD bytesWritten{};
                const auto written = WriteFile(
                    file,
                    defaultBytes.data(),
                    static_cast<DWORD>(defaultBytes.size()),
                    &bytesWritten,
                    nullptr);
                const auto flushed = written && FlushFileBuffers(file);
                CloseHandle(file);
                if (!flushed
                    || static_cast<std::size_t>(bytesWritten) != defaultBytes.size()) {
                    DeleteFileW(layoutPath.c_str());
                    Context->LogError(
                        "WebbrowserInGame: could not write the default BrowserPanelhd.json.");
                    return false;
                }
                Context->LogInfo(
                    "WebbrowserInGame: created editable BrowserPanelhd.json beside the plugin DLL.");
            }
        }

        if (!ReadBrowserPanelLayoutFile(layoutPath, bytes)) {
            Context->LogError(
                "WebbrowserInGame: BrowserPanelhd.json could not be read or exceeds 4 MiB.");
            return false;
        }
        return true;
    } catch (...) {
        Context->LogError(
            "WebbrowserInGame: failed while preparing BrowserPanelhd.json.");
        return false;
    }
}

bool RegisterLayoutResource(
        std::uint16_t resourceId,
        const char* path,
        D2RL::Resources::RegistrationHandle& handle,
        const char* label) {
    std::vector<std::uint8_t> bytes;
    if (!LoadEmbeddedLayout(resourceId, bytes)) {
        Context->LogError(label);
        return false;
    }

    const D2RL::Resources::ResourceRegistration registration{
        .structSize = D2RL::Resources::ResourceRegistrationSize,
        .flags = 0,
        .path = path,
        .bytes = bytes.data(),
        .byteCount = bytes.size(),
    };
    const auto result = ResourceService->registerResource(
        Context, &registration, &handle);
    if (result != D2RL::Resources::Result::Success
        || handle == D2RL::Resources::InvalidHandle) {
        handle = D2RL::Resources::InvalidHandle;
        Context->LogError(label);
        return false;
    }
    return true;
}

bool RegisterBrowserPanelLayoutResource() {
    std::vector<std::uint8_t> bytes;
    if (!LoadBrowserPanelLayoutFile(bytes)) return false;

    const D2RL::Resources::ResourceRegistration registration{
        .structSize = D2RL::Resources::ResourceRegistrationSize,
        .flags = 0,
        .path = BrowserPanelLayoutPath,
        .bytes = bytes.data(),
        .byteCount = bytes.size(),
    };
    const auto result = ResourceService->registerResource(
        Context, &registration, &BrowserPanelLayoutResource);
    if (result != D2RL::Resources::Result::Success
        || BrowserPanelLayoutResource == D2RL::Resources::InvalidHandle) {
        BrowserPanelLayoutResource = D2RL::Resources::InvalidHandle;
        Context->LogError(
            "WebbrowserInGame: browser panel layout resource registration failed.");
        return false;
    }
    Context->LogInfo(
        "WebbrowserInGame: registered the browser panel layout from BrowserPanelhd.json.");
    return true;
}

bool RegisterBrowserPanel() {
    const D2RL::Panels::PanelRegistration registration{
        .structSize = D2RL::Panels::PanelRegistrationSize,
        .flags = D2RL::Panels::PanelFlags::CloseOnEscape,
        .localId = BrowserPanelLocalId,
    };
    const auto result = PanelService->registerPanel(
        Context, &registration, &BrowserPanel);
    if (result != D2RL::Panels::Result::Success
        || BrowserPanel == D2RL::Panels::InvalidHandle) {
        BrowserPanel = D2RL::Panels::InvalidHandle;
        Context->LogError("WebbrowserInGame: native browser panel registration failed.");
        return false;
    }
    return true;
}

bool FindHtmlPath(std::wstring& pagePath) {
    std::wstring pluginDirectory;
    if (!ResolvePluginDirectory(pluginDirectory)) return false;

    std::wstring configuredHtmlFile;
    if (!Utf8ToWide(ConfiguredHtmlFile, configuredHtmlFile)) {
        Context->LogError(
            "WebbrowserInGame: html_file is not valid UTF-8.");
        return false;
    }

    auto configuredPath = std::filesystem::path(configuredHtmlFile);
    if (!configuredPath.is_absolute()) {
        if (configuredPath.has_root_name()
            || configuredPath.has_root_directory()) {
            Context->LogError(
                "WebbrowserInGame: html_file must be a relative path or a fully absolute path.");
            return false;
        }
        configuredPath = std::filesystem::path(pluginDirectory) / configuredPath;
    }
    pagePath = configuredPath.lexically_normal().native();
    if (!IsFile(pagePath)) {
        Context->LogError(
            "WebbrowserInGame: the file configured by html_file was not found.");
        return false;
    }
    return true;
}

bool FindUrlForPage(std::wstring& url) {
    std::wstring pagePath;
    if (!FindHtmlPath(pagePath)) return false;

    std::vector<wchar_t> urlBuffer(32768);
    DWORD length = static_cast<DWORD>(urlBuffer.size());
    const auto result = UrlCreateFromPathW(
        pagePath.c_str(), urlBuffer.data(), &length, 0);
    if (FAILED(result) || length == 0 || length >= urlBuffer.size()
        || urlBuffer[0] == L'\0') {
        Context->LogError("WebbrowserInGame: could not convert the HTML path to a file URL.");
        return false;
    }
    url.assign(urlBuffer.data());
    if (BrowserSessionToken == 0 || BrowserSessionProcessId == 0) {
        Context->LogError(
            "WebbrowserInGame: browser session identity is unavailable.");
        return false;
    }
    url += url.find(L'?') == std::wstring::npos ? L"?" : L"&";
    url += L"d2rl-session=";
    url += std::to_wstring(BrowserSessionToken);
    url += L"-";
    url += std::to_wstring(BrowserSessionProcessId);
    return true;
}

bool OpenConfiguredPageExternally() noexcept {
    try {
        std::wstring pagePath;
        if (!FindHtmlPath(pagePath)) return false;

        const auto result = ShellExecuteW(
            GameWindow, L"open", pagePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        const auto shellResult = reinterpret_cast<INT_PTR>(result);
        if (shellResult <= 32) {
            if (Context) {
                auto error = std::string(
                    "WebbrowserInGame: Windows could not open html_file externally (ShellExecute result ");
                error += std::to_string(shellResult);
                error += ").";
                Context->LogError(error.c_str());
            }
            return false;
        }

        if (Context) {
            Context->LogInfo(
                "WebbrowserInGame: opened the configured HTML file with its Windows default application.");
        }
        return true;
    } catch (...) {
        if (Context) {
            Context->LogError(
                "WebbrowserInGame: failed to open the configured HTML file externally.");
        }
        return false;
    }
}

struct GameWindowSearch {
    DWORD processId;
    HWND window;
    long long clientArea;
};

BOOL CALLBACK FindGameWindow(HWND window, LPARAM parameter) noexcept {
    auto* search = reinterpret_cast<GameWindowSearch*>(parameter);
    DWORD processId{};
    if (GetWindowThreadProcessId(window, &processId) == 0
        || processId != search->processId
        || !IsWindowVisible(window)
        || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }

    RECT client{};
    if (!GetClientRect(window, &client)) return TRUE;
    const auto width = client.right - client.left;
    const auto height = client.bottom - client.top;
    if (width < 640 || height < 480) return TRUE;

    const auto area = static_cast<long long>(width) * height;
    if (area > search->clientArea) {
        search->window = window;
        search->clientArea = area;
    }
    return TRUE;
}

HWND ResolveGameWindow() noexcept {
    GameWindowSearch search{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

LRESULT CALLBACK BrowserKeyboardHookProc(
        int code,
        WPARAM key,
        LPARAM keyData) noexcept {
    if (code < 0) {
        return CallNextHookEx(nullptr, code, key, keyData);
    }

    if (key == VK_ESCAPE
        && BrowserEscapeActive.load(std::memory_order_acquire)) {
        const auto bits = static_cast<std::uint32_t>(keyData);
        const bool keyUp = (bits & 0x80000000U) != 0;
        const bool wasAlreadyDown = (bits & 0x40000000U) != 0;
        if (!BrowserEscapeCloseQueued.load(std::memory_order_acquire)
            && !keyUp
            && !wasAlreadyDown) {
            bool expected = false;
            if (BrowserEscapeCloseQueued.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                if (EscapeMessageWindow && IsWindow(EscapeMessageWindow)
                    && PostMessageW(
                        EscapeMessageWindow, NativePanelCloseMessage, 0, 0)) {
                    return 1;
                }
                BrowserEscapeCloseQueued.store(
                    false, std::memory_order_release);
            }
        }
        if (BrowserEscapeCloseQueued.load(std::memory_order_acquire)) {
            return 1;
        }
    }

    return CallNextHookEx(nullptr, code, key, keyData);
}

bool InstallBrowserEscapeHook() noexcept {
    if (BrowserEscapeHook) return true;
    if (!GameWindow || !IsWindow(GameWindow)) return false;

    DWORD processId{};
    const auto threadId = GetWindowThreadProcessId(GameWindow, &processId);
    if (threadId == 0 || processId != GetCurrentProcessId()) {
        if (Context) {
            Context->LogError(
                "WebbrowserInGame: Escape hook refused an unvalidated game host thread.");
        }
        return false;
    }

    BrowserEscapeCloseQueued.store(false, std::memory_order_release);
    BrowserEscapeHook = SetWindowsHookExW(
        WH_KEYBOARD, BrowserKeyboardHookProc, nullptr, threadId);
    if (!BrowserEscapeHook) {
        if (Context) {
            Context->LogError(
                "WebbrowserInGame: could not install the game-thread Escape hook.");
        }
        return false;
    }
    BrowserEscapeActive.store(true, std::memory_order_release);
    if (Context) {
        Context->LogInfo(
            "WebbrowserInGame: installed a thread-scoped Escape filter while CEF is open.");
    }
    return true;
}

void RemoveBrowserEscapeHook() noexcept {
    BrowserEscapeActive.store(false, std::memory_order_release);
    BrowserEscapeCloseQueued.store(false, std::memory_order_release);
    const auto hook = BrowserEscapeHook;
    BrowserEscapeHook = nullptr;
    if (hook && !UnhookWindowsHookEx(hook) && Context) {
        Context->LogWarn(
            "WebbrowserInGame: could not remove the game-thread Escape hook.");
    }
}

struct BrowserWindowSearch {
    DWORD processId;
    HWND window;
};

BOOL CALLBACK FindBrowserWindow(HWND window, LPARAM parameter) noexcept {
    auto* search = reinterpret_cast<BrowserWindowSearch*>(parameter);
    DWORD ownerProcessId{};
    if (GetWindowThreadProcessId(window, &ownerProcessId) != 0
        && ownerProcessId == search->processId
        && GetWindow(window, GW_OWNER) == GameWindow
        && IsWindowVisible(window)) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindBrowserWindowForProcess() noexcept {
    if (!GameWindow || BrowserProcessId == 0) return nullptr;
    BrowserWindowSearch search{BrowserProcessId, nullptr};
    EnumWindows(FindBrowserWindow, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

bool GetBrowserBounds(RECT& bounds) noexcept {
    if (!GameWindow || !IsWindow(GameWindow)) return false;
    RECT client{};
    if (!GetClientRect(GameWindow, &client)) return false;
    const auto clientWidth = client.right - client.left;
    const auto clientHeight = client.bottom - client.top;
    if (clientWidth < 640 || clientHeight < 480) return false;

    // Match the Options panel's content area on the 3200x1800 HD canvas.
    // Leave room for its title and close button while filling the body down
    // to the bottom frame margin.
    constexpr double CanvasWidth = 3200.0;
    constexpr double CanvasHeight = 1800.0;
    constexpr double ContentX = 846.0;
    constexpr double ContentY = 227.0;
    constexpr double ContentWidth = 1503.0;
    constexpr double ContentHeight = 1180.0;
    const auto scaleX = static_cast<double>(clientWidth) / CanvasWidth;
    const auto scaleY = static_cast<double>(clientHeight) / CanvasHeight;
    bounds.left = static_cast<LONG>(std::lround(ContentX * scaleX));
    bounds.top = static_cast<LONG>(std::lround(ContentY * scaleY));
    bounds.right = bounds.left
        + static_cast<LONG>(std::lround(ContentWidth * scaleX));
    bounds.bottom = bounds.top
        + static_cast<LONG>(std::lround(ContentHeight * scaleY));
    if (bounds.left < 0 || bounds.top < 0
        || bounds.right > clientWidth || bounds.bottom > clientHeight
        || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return false;
    }

    POINT topLeft{bounds.left, bounds.top};
    POINT bottomRight{bounds.right, bounds.bottom};
    if (!ClientToScreen(GameWindow, &topLeft)
        || !ClientToScreen(GameWindow, &bottomRight)) {
        return false;
    }
    bounds = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    return true;
}

void BringBrowserForward() noexcept {
    if (!BrowserProcess || BrowserProcessId == 0 || BrowserCloseRequested) return;
    if (WaitForSingleObject(BrowserProcess, 0) != WAIT_TIMEOUT) return;
    if (const auto browserWindow = FindBrowserWindowForProcess()) {
        ShowWindow(browserWindow, SW_SHOW);
        SetWindowPos(browserWindow, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(browserWindow);
    }
}

void RequestBrowserClose() noexcept;
void QueueBrowserClose() noexcept;
void BeginNativePanelClose() noexcept;
void CloseNativeBrowserPanel() noexcept;
void PollBrowserLifecycle() noexcept;
LRESULT CALLBACK EscapeMessageWindowProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) noexcept {
    if (message == NativePanelOpenMessage) {
        if (Context && PanelService && BrowserPanel != D2RL::Panels::InvalidHandle) {
            const auto result = PanelService->openPanel(Context, BrowserPanel);
            if (result != D2RL::Panels::Result::Success) {
                Context->LogWarn(
                    "WebbrowserInGame: SDK could not open the native panel.");
                QueueBrowserClose();
            } else if (Context) {
                BrowserPanelOpen = true;
                Context->LogInfo("WebbrowserInGame: SDK opened the native panel.");
            }
        }
        return 0;
    }

    if (message == NativePanelCloseMessage) {
        BeginNativePanelClose();
        return 0;
    }

    if (message == NativePanelOpenExternalMessage) {
        if (!BrowserPanelOpen || !BrowserProcess
            || WaitForSingleObject(BrowserProcess, 0) != WAIT_TIMEOUT) {
            if (Context) {
                Context->LogWarn(
                    "WebbrowserInGame: ignored the external-open action because the in-game browser is not open.");
            }
            return 0;
        }

        if (OpenConfiguredPageExternally()) BeginNativePanelClose();
        return 0;
    }

    if (message == BrowserReadyMessage) {
        const auto controlWindow = reinterpret_cast<HWND>(wParam);
        DWORD ownerProcessId{};
        if (BrowserProcess && controlWindow && IsWindow(controlWindow)
            && GetWindowThreadProcessId(controlWindow, &ownerProcessId) != 0
            && ownerProcessId == BrowserProcessId) {
            BrowserControlWindow = controlWindow;
            BrowserReady = true;
            if (Context) {
                Context->LogInfo("WebbrowserInGame: CEF close control connected.");
            }
            BringBrowserForward();
            if (BrowserCloseRequested) RequestBrowserClose();
        } else if (Context) {
            Context->LogWarn(
                "WebbrowserInGame: ignored an invalid CEF control window.");
        }
        return 0;
    }

    if (message == WM_TIMER && wParam == BrowserCloseRetryTimerId) {
        PollBrowserLifecycle();
        return 0;
    }

    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }

    if (message == WM_NCDESTROY) {
        if (EscapeMessageWindow == window) {
            EscapeMessageWindow = nullptr;
            EscapeMessageWindowThreadId = 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool EnsureEscapeMessageWindow() noexcept {
    if (EscapeMessageWindow && IsWindow(EscapeMessageWindow)) {
        return GetCurrentThreadId() == EscapeMessageWindowThreadId;
    }

    const auto module = reinterpret_cast<HINSTANCE>(&__ImageBase);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = EscapeMessageWindowProc;
    windowClass.hInstance = module;
    windowClass.lpszClassName = EscapeMessageWindowClass;
    if (!RegisterClassExW(&windowClass)
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (Context) {
            Context->LogError(
                "WebbrowserInGame: Escape message window class registration failed.");
        }
        return false;
    }

    EscapeMessageWindow = CreateWindowExW(
        0,
        EscapeMessageWindowClass,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        module,
        nullptr);
    if (!EscapeMessageWindow) {
        UnregisterClassW(EscapeMessageWindowClass, module);
        if (Context) {
            Context->LogError(
                "WebbrowserInGame: Escape message window creation failed.");
        }
        return false;
    }

    EscapeMessageWindowThreadId = GetCurrentThreadId();
    return true;
}

void DestroyEscapeMessageWindow() noexcept {
    RemoveBrowserEscapeHook();
    const auto window = EscapeMessageWindow;
    const auto module = reinterpret_cast<HINSTANCE>(&__ImageBase);
    if (!window || !IsWindow(window)) {
        EscapeMessageWindow = nullptr;
        EscapeMessageWindowThreadId = 0;
        UnregisterClassW(EscapeMessageWindowClass, module);
        return;
    }

    if (GetCurrentThreadId() == EscapeMessageWindowThreadId) {
        SendMessageW(window, WM_CLOSE, 0, 0);
    } else {
        DWORD_PTR result{};
        if (!SendMessageTimeoutW(
                window,
                WM_CLOSE,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                1000,
                &result)) {
            if (Context) {
                Context->LogWarn(
                    "WebbrowserInGame: Escape message window cleanup did not complete.");
            }
            return;
        }
    }

    UnregisterClassW(EscapeMessageWindowClass, module);
}

bool LaunchBrowser() {
    if (BrowserProcess) {
        if (WaitForSingleObject(BrowserProcess, 0) == WAIT_TIMEOUT) {
            if (BrowserCloseRequested) {
                Context->LogWarn(
                    "WebbrowserInGame: the previous browser window is still closing; try again shortly.");
                return false;
            }
            BringBrowserForward();
            return true;
        }
        RemoveBrowserEscapeHook();
        if (EscapeMessageWindow && IsWindow(EscapeMessageWindow)) {
            KillTimer(EscapeMessageWindow, BrowserCloseRetryTimerId);
        }
        CloseHandle(BrowserProcess);
        BrowserProcess = nullptr;
        BrowserProcessId = 0;
        BrowserControlWindow = nullptr;
        BrowserCloseRequested = false;
        BrowserCloseMessageSent = false;
        BrowserReady = false;
        BrowserPanelOpen = false;
    }

    GameWindow = ResolveGameWindow();
    RECT browserBounds{};
    if (!GameWindow || !GetBrowserBounds(browserBounds)
        || !EscapeMessageWindow || !IsWindow(EscapeMessageWindow)) {
        Context->LogError(
            "WebbrowserInGame: D2R host window or supported panel bounds could not be resolved.");
        return false;
    }

    std::wstring pluginDirectory;
    if (!ResolvePluginDirectory(pluginDirectory)) {
        Context->LogError(
            "WebbrowserInGame: plugin directory could not be resolved.");
        return false;
    }
    const auto cefDirectory = pluginDirectory + L"\\CEF corefiles";
    const auto executable = cefDirectory + L"\\cefsimple.exe";
    const auto clientDll = cefDirectory + L"\\cefsimple.dll";
    const auto cefLibrary = cefDirectory + L"\\libcef.dll";
    if (!IsFile(executable) || !IsFile(clientDll) || !IsFile(cefLibrary)) {
        Context->LogError(
            "WebbrowserInGame: CEF corefiles is incomplete beside the plugin DLL.");
        return false;
    }

    std::wstring url;
    if (!FindUrlForPage(url)) return false;

    std::wstring commandLine = L"\"" + executable + L"\" --url=\"" + url
        + L"\" --parent-hwnd="
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(GameWindow))
        + L" --escape-hwnd="
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(EscapeMessageWindow))
        + L" --x=" + std::to_wstring(browserBounds.left)
        + L" --y=" + std::to_wstring(browserBounds.top)
        + L" --width=" + std::to_wstring(browserBounds.right - browserBounds.left)
        + L" --height=" + std::to_wstring(browserBounds.bottom - browserBounds.top);
    std::vector<wchar_t> mutableCommandLine(
        commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(
            executable.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            cefDirectory.c_str(),
            &startupInfo,
            &processInfo)) {
        Context->LogError("WebbrowserInGame: CreateProcessW could not start cefsimple.exe.");
        return false;
    }

    CloseHandle(processInfo.hThread);
    BrowserProcess = processInfo.hProcess;
    BrowserProcessId = processInfo.dwProcessId;
    BrowserControlWindow = nullptr;
    BrowserCloseRequested = false;
    BrowserCloseMessageSent = false;
    ClosePanelAfterBrowserExit = false;
    BrowserReady = false;
    BrowserPanelOpen = false;
    if (!InstallBrowserEscapeHook()) {
        RequestBrowserClose();
        return false;
    }
    Context->LogInfo("WebbrowserInGame: opened the configured HTML page in the native-style panel.");
    if (SetTimer(EscapeMessageWindow, BrowserCloseRetryTimerId, 100, nullptr) == 0) {
        Context->LogWarn(
            "WebbrowserInGame: could not start browser and panel lifecycle monitoring.");
    }
    return true;
}

auto __cdecl OnButtonMessage(
        const D2RL::PluginContext*,
        const D2RL::SharedEvents::UiMessageEvent* event,
        void*) noexcept -> D2RL::SharedEvents::UiMessageAction {
    if (!event
        || event->structSize < D2RL::SharedEvents::UiMessageEventRequiredSize
        || !event->target
        || !event->command
        || !event->text
        || std::strcmp(event->target, ClickMessageTarget) != 0) {
        return D2RL::SharedEvents::UiMessageAction::Continue;
    }

    try {
        if (std::strcmp(event->command, ClosePanelCommand) == 0
            && std::strcmp(event->text, OpenExternalPageAction) == 0) {
            if (!EscapeMessageWindow || !IsWindow(EscapeMessageWindow)
                || !PostMessageW(
                    EscapeMessageWindow, NativePanelOpenExternalMessage, 0, 0)) {
                if (Context) {
                    Context->LogWarn(
                        "WebbrowserInGame: could not queue the external browser action.");
                }
            }
            return D2RL::SharedEvents::UiMessageAction::Consume;
        }

        if (std::strcmp(event->text, BrowserPanelName) == 0
            && std::strcmp(event->command, TogglePanelCommand) == 0) {
            D2RL::Panels::PanelInfo info{
                .structSize = D2RL::Panels::PanelInfoSize,
                .flags = D2RL::Panels::PanelFlags::None,
                .handle = D2RL::Panels::InvalidHandle,
                .ownerGeneration = 0,
                .registrationState = D2RL::Panels::RegistrationState::Unknown,
                .presentationState = D2RL::Panels::PresentationState::Unknown,
                .reserved = 0,
            };
            const auto result = PanelService->getPanelInfo(
                Context, BrowserPanel, &info);
            if (result != D2RL::Panels::Result::Success
                || (info.presentationState != D2RL::Panels::PresentationState::Open
                    && info.presentationState != D2RL::Panels::PresentationState::Closed)) {
                Context->LogWarn(
                    "WebbrowserInGame: panel state is unavailable; ignoring the toggle.");
                return D2RL::SharedEvents::UiMessageAction::Consume;
            }
            if (info.presentationState == D2RL::Panels::PresentationState::Open) {
                QueueBrowserClose();
                Context->LogInfo(
                    "WebbrowserInGame: consumed panel toggle and queued ordered CEF/panel shutdown.");
                return D2RL::SharedEvents::UiMessageAction::Consume;
            } else if (!EnsureEscapeMessageWindow() || !LaunchBrowser()) {
                return D2RL::SharedEvents::UiMessageAction::Consume;
            }
            if (EscapeMessageWindow && IsWindow(EscapeMessageWindow)
                && PostMessageW(EscapeMessageWindow, NativePanelOpenMessage, 0, 0)) {
                return D2RL::SharedEvents::UiMessageAction::Consume;
            }
            Context->LogWarn(
                "WebbrowserInGame: could not queue native panel open.");
            QueueBrowserClose();
            return D2RL::SharedEvents::UiMessageAction::Consume;
        }
    } catch (...) {
        if (Context) {
            Context->LogError("WebbrowserInGame: browser panel action failed unexpectedly.");
        }
    }
    return D2RL::SharedEvents::UiMessageAction::Continue;
}

bool RegisterButtonListener() {
    const D2RL::SharedEvents::UiMessageListener listener{
        .structSize = D2RL::SharedEvents::UiMessageListenerSize,
        .flags = 0,
        .priority = 10'000,
        .reserved = 0,
        .callback = OnButtonMessage,
        .userData = nullptr,
    };
    const auto result = SharedEventService->registerUiMessageListener(
        Context, &listener, &UiMessageListener);
    if (result != D2RL::SharedEvents::Result::Success
        || UiMessageListener == D2RL::SharedEvents::InvalidHandle) {
        UiMessageListener = D2RL::SharedEvents::InvalidHandle;
        Context->LogError("WebbrowserInGame: UI button listener registration failed.");
        return false;
    }
    return true;
}

void RequestBrowserClose() noexcept {
    if (!BrowserProcess) {
        if (EscapeMessageWindow && IsWindow(EscapeMessageWindow)) {
            KillTimer(EscapeMessageWindow, BrowserCloseRetryTimerId);
        }
        if (ClosePanelAfterBrowserExit) CloseNativeBrowserPanel();
        return;
    }

    const auto wait = WaitForSingleObject(BrowserProcess, 0);
    if (wait == WAIT_OBJECT_0) {
        RemoveBrowserEscapeHook();
        const bool unexpectedExit = !BrowserCloseRequested;
        DWORD exitCode{};
        (void)GetExitCodeProcess(BrowserProcess, &exitCode);
        if (EscapeMessageWindow && IsWindow(EscapeMessageWindow)) {
            KillTimer(EscapeMessageWindow, BrowserCloseRetryTimerId);
        }
        if (unexpectedExit && Context) {
            auto error = std::string(
                "WebbrowserInGame: CEF exited unexpectedly with code ");
            error += std::to_string(exitCode);
            error += ". See CEF corefiles\\cef-debug.log.";
            Context->LogError(error.c_str());
        }
        CloseHandle(BrowserProcess);
        BrowserProcess = nullptr;
        BrowserProcessId = 0;
        BrowserControlWindow = nullptr;
        BrowserCloseRequested = false;
        BrowserCloseMessageSent = false;
        BrowserReady = false;
        if (!ClosePanelAfterBrowserExit && unexpectedExit && BrowserPanelOpen) {
            ClosePanelAfterBrowserExit = true;
        }
        if (ClosePanelAfterBrowserExit || (unexpectedExit && BrowserPanelOpen)) {
            CloseNativeBrowserPanel();
        }
        return;
    }
    if (wait == WAIT_FAILED) {
        if (Context) {
            Context->LogWarn(
                "WebbrowserInGame: could not query the CEF process during shutdown.");
        }
        return;
    }

    BrowserCloseRequested = true;
    if (!BrowserCloseMessageSent) {
        if (!BrowserControlWindow || !IsWindow(BrowserControlWindow)) {
            BrowserControlWindow = nullptr;
        } else {
            DWORD ownerProcessId{};
            if (GetWindowThreadProcessId(
                    BrowserControlWindow, &ownerProcessId) == 0
                || ownerProcessId != BrowserProcessId) {
                BrowserControlWindow = nullptr;
            } else if (PostMessageW(
                    BrowserControlWindow, CefCloseBrowserMessage, 0, 0)) {
                BrowserCloseMessageSent = true;
                if (Context) {
                    Context->LogInfo(
                        "WebbrowserInGame: sent graceful CEF CloseBrowser request.");
                }
            }
        }
    }

    if (EscapeMessageWindow && IsWindow(EscapeMessageWindow)
        && SetTimer(EscapeMessageWindow, BrowserCloseRetryTimerId, 100, nullptr) == 0
        && Context) {
        Context->LogWarn(
            "WebbrowserInGame: could not schedule CEF close completion polling.");
    }
}

void CloseNativeBrowserPanel() noexcept {
    ClosePanelAfterBrowserExit = false;
    if (!Context || !PanelService
        || BrowserPanel == D2RL::Panels::InvalidHandle) {
        return;
    }

    D2RL::Panels::PanelInfo info{
        .structSize = D2RL::Panels::PanelInfoSize,
        .flags = D2RL::Panels::PanelFlags::None,
        .handle = D2RL::Panels::InvalidHandle,
        .ownerGeneration = 0,
        .registrationState = D2RL::Panels::RegistrationState::Unknown,
        .presentationState = D2RL::Panels::PresentationState::Unknown,
        .reserved = 0,
    };
    if (PanelService->getPanelInfo(Context, BrowserPanel, &info)
            != D2RL::Panels::Result::Success) {
        return;
    }
    if (info.presentationState != D2RL::Panels::PresentationState::Open) {
        BrowserPanelOpen = false;
        ClosePanelAfterBrowserExit = false;
        return;
    }

    const auto result = PanelService->closePanel(Context, BrowserPanel);
    if (result == D2RL::Panels::Result::Success) {
        BrowserPanelOpen = false;
        if (Context) {
            Context->LogInfo(
                "WebbrowserInGame: closed the native panel after CEF exited.");
        }
    } else if (Context) {
        Context->LogWarn(
            "WebbrowserInGame: SDK could not close the native browser panel.");
    }
}

void BeginNativePanelClose() noexcept {
    BrowserEscapeCloseQueued.store(true, std::memory_order_release);
    ClosePanelAfterBrowserExit = true;
    if (const auto browserWindow = FindBrowserWindowForProcess()) {
        ShowWindow(browserWindow, SW_HIDE);
    }
    RequestBrowserClose();
}

void PollBrowserLifecycle() noexcept {
    if (!BrowserProcess) {
        if (EscapeMessageWindow && IsWindow(EscapeMessageWindow)) {
            KillTimer(EscapeMessageWindow, BrowserCloseRetryTimerId);
        }
        return;
    }

    const auto wait = WaitForSingleObject(BrowserProcess, 0);
    if (wait != WAIT_TIMEOUT) {
        RequestBrowserClose();
        return;
    }

    if (!BrowserPanelOpen || !Context || !PanelService
        || BrowserPanel == D2RL::Panels::InvalidHandle) {
        if (BrowserCloseRequested) RequestBrowserClose();
        return;
    }

    D2RL::Panels::PanelInfo info{
        .structSize = D2RL::Panels::PanelInfoSize,
        .flags = D2RL::Panels::PanelFlags::None,
        .handle = D2RL::Panels::InvalidHandle,
        .ownerGeneration = 0,
        .registrationState = D2RL::Panels::RegistrationState::Unknown,
        .presentationState = D2RL::Panels::PresentationState::Unknown,
        .reserved = 0,
    };
    if (PanelService->getPanelInfo(Context, BrowserPanel, &info)
            == D2RL::Panels::Result::Success
        && info.presentationState == D2RL::Panels::PresentationState::Closed) {
        BrowserPanelOpen = false;
        ClosePanelAfterBrowserExit = false;
        BrowserCloseRequested = true;
        if (const auto browserWindow = FindBrowserWindowForProcess()) {
            ShowWindow(browserWindow, SW_HIDE);
        }
    }
    if (BrowserCloseRequested) RequestBrowserClose();
}

void QueueBrowserClose() noexcept {
    if (EscapeMessageWindow && IsWindow(EscapeMessageWindow)
        && PostMessageW(EscapeMessageWindow, NativePanelCloseMessage, 0, 0)) {
        return;
    }

    if (Context) {
        Context->LogWarn(
            "WebbrowserInGame: could not queue browser shutdown after panel close.");
    }
}

void UnregisterUi() noexcept {
    RemoveBrowserEscapeHook();
    if (BrowserProcess) {
        RequestBrowserClose();
        const auto waitResult = WaitForSingleObject(BrowserProcess, 1500);
        if (waitResult == WAIT_TIMEOUT && Context) {
            Context->LogWarn(
                "WebbrowserInGame: CEF did not exit before plugin teardown completed.");
        }
        CloseHandle(BrowserProcess);
        BrowserProcess = nullptr;
        BrowserProcessId = 0;
        BrowserControlWindow = nullptr;
        BrowserCloseRequested = false;
        BrowserCloseMessageSent = false;
    }
    DestroyEscapeMessageWindow();

    if (Context && SharedEventService
        && UiMessageListener != D2RL::SharedEvents::InvalidHandle) {
        (void)SharedEventService->unregisterUiMessageListener(
            Context, UiMessageListener);
    }
    UiMessageListener = D2RL::SharedEvents::InvalidHandle;

    if (Context && PanelService
        && BrowserPanel != D2RL::Panels::InvalidHandle) {
        (void)PanelService->unregisterPanel(Context, BrowserPanel);
    }
    BrowserPanel = D2RL::Panels::InvalidHandle;

    if (Context && ResourceService
        && BrowserPanelLayoutResource != D2RL::Resources::InvalidHandle) {
        (void)ResourceService->unregisterResource(
            Context, BrowserPanelLayoutResource);
    }
    BrowserPanelLayoutResource = D2RL::Resources::InvalidHandle;

    if (Context && ResourceService
        && OpenExternalButtonResource != D2RL::Resources::InvalidHandle) {
        (void)ResourceService->unregisterResource(
            Context, OpenExternalButtonResource);
    }
    OpenExternalButtonResource = D2RL::Resources::InvalidHandle;

    if (Context && ResourceService
        && OpenExternalButtonLowEndResource != D2RL::Resources::InvalidHandle) {
        (void)ResourceService->unregisterResource(
            Context, OpenExternalButtonLowEndResource);
    }
    OpenExternalButtonLowEndResource = D2RL::Resources::InvalidHandle;

    ClosePanelAfterBrowserExit = false;
}

bool QueryServices() noexcept {
    const void* service = nullptr;
    const auto resourceResult = Context->QueryService(
        D2RL::ResourceService::Id,
        D2RL::ResourceService::AbiVersion,
        &service);
    ResourceService = static_cast<const D2RL::ResourceService*>(service);
    if (resourceResult != D2RL::ServiceQueryResult::Success
        || !D2RL::HasResourceServiceField(
            ResourceService, D2RL::ResourceServiceRequiredSize)
        || !ResourceService->registerResource
        || !ResourceService->unregisterResource) {
        Context->LogError("WebbrowserInGame: D2RLoader Resource service v1 is unavailable.");
        return false;
    }

    service = nullptr;
    const auto panelResult = Context->QueryService(
        D2RL::PanelService::Id,
        D2RL::PanelService::AbiVersion,
        &service);
    PanelService = static_cast<const D2RL::PanelService*>(service);
    if (panelResult != D2RL::ServiceQueryResult::Success
        || !D2RL::HasPanelServiceField(
            PanelService, D2RL::PanelServiceRegisterPanelFieldEnd)
        || !D2RL::HasPanelServiceField(
            PanelService, D2RL::PanelServiceUnregisterPanelFieldEnd)
        || !D2RL::HasPanelServiceField(
            PanelService, D2RL::PanelServiceGetPanelInfoFieldEnd)
        || !D2RL::HasPanelServiceField(
            PanelService, D2RL::PanelServiceOpenPanelFieldEnd)
        || !D2RL::HasPanelServiceField(
            PanelService, D2RL::PanelServiceClosePanelFieldEnd)
        || !PanelService->registerPanel
        || !PanelService->unregisterPanel
        || !PanelService->getPanelInfo
        || !PanelService->openPanel
        || !PanelService->closePanel) {
        Context->LogError("WebbrowserInGame: D2RLoader Panel service v1 is unavailable.");
        return false;
    }

    service = nullptr;
    const auto sharedEventResult = Context->QueryService(
        D2RL::SharedEventService::Id,
        D2RL::SharedEventService::AbiVersion,
        &service);
    SharedEventService = static_cast<const D2RL::SharedEventService*>(service);
    if (sharedEventResult != D2RL::ServiceQueryResult::Success
        || !D2RL::HasSharedEventServiceField(
            SharedEventService, D2RL::SharedEventServiceRequiredSize)
        || !SharedEventService->registerUiMessageListener
        || !SharedEventService->unregisterUiMessageListener) {
        Context->LogError("WebbrowserInGame: D2RLoader SharedEvent service v1 is unavailable.");
        return false;
    }

    return true;
}

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept
    -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(
        const D2RL::PluginContext* context) noexcept -> bool {
    if (!D2RL::HasContext(context)
        || context->abiVersion < D2RL_PLUGIN_ABI_VERSION) {
        return false;
    }

    Context = context;
    ResourceService = nullptr;
    PanelService = nullptr;
    SharedEventService = nullptr;
    BrowserPanelLayoutResource = D2RL::Resources::InvalidHandle;
    OpenExternalButtonResource = D2RL::Resources::InvalidHandle;
    OpenExternalButtonLowEndResource = D2RL::Resources::InvalidHandle;
    BrowserPanel = D2RL::Panels::InvalidHandle;
    UiMessageListener = D2RL::SharedEvents::InvalidHandle;
    BrowserProcess = nullptr;
    BrowserProcessId = 0;
    FILETIME sessionStart{};
    GetSystemTimeAsFileTime(&sessionStart);
    BrowserSessionToken =
        (static_cast<std::uint64_t>(sessionStart.dwHighDateTime) << 32U)
        | sessionStart.dwLowDateTime;
    if (BrowserSessionToken == 0) BrowserSessionToken = GetTickCount64();
    BrowserSessionProcessId = GetCurrentProcessId();
    BrowserControlWindow = nullptr;
    BrowserCloseRequested = false;
    BrowserCloseMessageSent = false;
    ClosePanelAfterBrowserExit = false;
    EscapeMessageWindow = nullptr;
    EscapeMessageWindowThreadId = 0;
    BrowserEscapeHook = nullptr;
    BrowserEscapeActive.store(false, std::memory_order_release);
    BrowserEscapeCloseQueued.store(false, std::memory_order_release);
    GameWindow = nullptr;
    ConfiguredHtmlFile.clear();
    if (!QueryServices()) {
        Context = nullptr;
        return false;
    }
    if (!ReadConfiguration()
        || !RegisterBrowserPanelLayoutResource()
        || !RegisterLayoutResource(
            OpenExternalButtonResourceId,
            OpenExternalButtonResourcePath,
            OpenExternalButtonResource,
            "WebbrowserInGame: external-open button sprite registration failed.")
        || !RegisterLayoutResource(
            OpenExternalButtonLowEndResourceId,
            OpenExternalButtonLowEndResourcePath,
            OpenExternalButtonLowEndResource,
            "WebbrowserInGame: low-end external-open button sprite registration failed.")
        || !RegisterBrowserPanel()
        || !RegisterButtonListener()) {
        UnregisterUi();
        Context = nullptr;
        ResourceService = nullptr;
        PanelService = nullptr;
        SharedEventService = nullptr;
        return false;
    }

    context->LogInfo(
        "Webbrowser In-Game Plugin 0.2.36 loaded; the browser panel is ready.");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    RequestBrowserClose();
    UnregisterUi();
    SharedEventService = nullptr;
    PanelService = nullptr;
    ResourceService = nullptr;
    Context = nullptr;
}
