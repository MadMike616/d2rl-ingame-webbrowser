#include <windows.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_frame.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_sandbox_win.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <vector>

namespace {

std::wstring GetExecutablePath() {
    std::vector<wchar_t> path(32768);
    const auto count = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (count == 0 || count >= path.size()) return {};
    return {path.data(), count};
}

void WriteStartupLog(const char* message) noexcept {
    const auto file = CreateFileW(
        L"cef-startup.log",
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written{};
    (void)WriteFile(
        file, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
    static constexpr char newline[] = "\r\n";
    (void)WriteFile(file, newline, 2, &written, nullptr);
    CloseHandle(file);
}

class BrowserClient;

constexpr wchar_t BrowserControlWindowClass[] =
    L"MadMike.WebbrowserInGame.CefControlWindow";
constexpr UINT BrowserReadyMessage = WM_APP + 0x4B4;
constexpr UINT CefCloseBrowserMessage = WM_APP + 0x4B5;
constexpr UINT NativePanelCloseMessage = WM_APP + 0x4B8;

class BrowserClient final : public CefClient,
                            public CefLifeSpanHandler,
                            public CefKeyboardHandler,
                            public CefLoadHandler {
public:
    explicit BrowserClient(HWND escapeMessageWindow)
        : escapeMessageWindow_(escapeMessageWindow) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override {
        return this;
    }

    CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }

    bool OnPreKeyEvent(
            CefRefPtr<CefBrowser>,
            const CefKeyEvent& event,
            CefEventHandle,
            bool*) override {
        if (event.type == KEYEVENT_RAWKEYDOWN
            && event.windows_key_code == VK_ESCAPE
            && escapeMessageWindow_
            && IsWindow(escapeMessageWindow_)) {
            return PostMessageW(
                escapeMessageWindow_, NativePanelCloseMessage, 0, 0) != FALSE;
        }
        return false;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        browser_ = browser;
        if (!CreateControlWindow()) {
            browser_->GetHost()->CloseBrowser(true);
        }
    }

    void OnLoadEnd(
            CefRefPtr<CefBrowser>,
            CefRefPtr<CefFrame> frame,
            int) override {
        if (!frame || !frame->IsMain()) return;

        static constexpr char EdgeShadeScript[] = R"JS(
(() => {
  const id = 'd2rl-cef-edge-shade';
  let shade = document.getElementById(id);
  if (!shade) {
    shade = document.createElement('div');
    shade.id = id;
    shade.setAttribute('aria-hidden', 'true');
    shade.style.cssText =
      'position:fixed!important;inset:0!important;z-index:2147483647!important;'
      + 'pointer-events:none!important;'
      + 'box-shadow:inset 0 0 14px 3px rgba(0,0,0,.48),'
      + 'inset 0 0 1px 1px rgba(0,0,0,.65)!important;';
    document.documentElement.appendChild(shade);
  }
})();
)JS";
        frame->ExecuteJavaScript(EdgeShadeScript, frame->GetURL(), 0);

        static constexpr char PersistentCursorScript[] = R"JS(
(() => {
  const cursorId = 'd2rl-cef-persistent-cursor';
  if (document.getElementById(cursorId)) return;

  const style = document.createElement('style');
  style.textContent =
    'html.d2rl-cef-idle-pointer, html.d2rl-cef-idle-pointer * {'
    + 'cursor: none !important; }';
  (document.head || document.documentElement).appendChild(style);

  const cursor = document.createElement('div');
  cursor.id = cursorId;
  cursor.setAttribute('aria-hidden', 'true');
  cursor.style.cssText =
    'position:fixed!important;left:0!important;top:0!important;'
    + 'width:18px!important;height:24px!important;'
    + 'z-index:2147483647!important;pointer-events:none!important;'
    + 'visibility:hidden!important;will-change:transform!important;';

  const arrow = '<svg xmlns="http://www.w3.org/2000/svg" width="18" height="18" viewBox="0 0 28 34">'
    + '<path d="M2 1v25l6-6 5 12 4-2-5-12 9 0z" fill="#f4edda" stroke="#171411" stroke-width="2" stroke-linejoin="round"/>'
    + '</svg>';
  const textCursor = '<svg xmlns="http://www.w3.org/2000/svg" width="18" height="24" viewBox="0 0 28 34">'
    + '<path d="M14 5v24M9 5h10M9 29h10" fill="none" stroke="#171411" stroke-width="5"/>'
    + '<path d="M14 5v24M9 5h10M9 29h10" fill="none" stroke="#f4edda" stroke-width="2"/>'
    + '</svg>';
  const hand = '<svg xmlns="http://www.w3.org/2000/svg" width="18" height="24" viewBox="0 0 28 34">'
    + '<path d="M9 13V5a2 2 0 0 1 4 0v7V4a2 2 0 0 1 4 0v8V6a2 2 0 0 1 4 0v7V9a2 2 0 0 1 4 0v9c0 6-3 10-9 10h-2c-3 0-5-1-7-4l-4-6a2 2 0 0 1 3-3l3 3v-5a2 2 0 0 1 4 0z" fill="#f4edda" stroke="#171411" stroke-width="2" stroke-linejoin="round"/>'
    + '</svg>';

  document.documentElement.appendChild(cursor);

  let cursorShape = '';
  let idleTimer = 0;
  let cursorRevision = 0;
  const update = event => {
    const revision = ++cursorRevision;
    const target = event.target instanceof Element
      ? event.target
      : document.documentElement;
    const kind = getComputedStyle(target).cursor;
    const isText = kind === 'text' || kind === 'vertical-text';
    const isHand = kind === 'pointer' || kind === 'grab' || kind === 'grabbing';
    const nextShape = isText ? 'text' : isHand ? 'hand' : 'arrow';
    if (nextShape !== cursorShape) {
      cursor.innerHTML = nextShape === 'text' ? textCursor
        : nextShape === 'hand' ? hand
        : arrow;
      cursorShape = nextShape;
    }
    const hotX = isText ? 9 : isHand ? 5.79 : 1.29;
    const hotY = isText ? 12 : isHand ? 3.53 : 0.53;
    cursor.style.transform =
      `translate3d(${event.clientX - hotX}px,${event.clientY - hotY}px,0)`;
    document.documentElement.classList.remove('d2rl-cef-idle-pointer');
    cursor.style.visibility = 'hidden';
    window.clearTimeout(idleTimer);
    idleTimer = window.setTimeout(() => {
      cursor.style.visibility = 'visible';
      window.requestAnimationFrame(() => {
        if (revision !== cursorRevision) return;
        window.requestAnimationFrame(() => {
          if (revision !== cursorRevision) return;
          document.documentElement.classList.add('d2rl-cef-idle-pointer');
        });
      });
    }, 16);
  };
  const hide = () => {
    ++cursorRevision;
    window.clearTimeout(idleTimer);
    document.documentElement.classList.remove('d2rl-cef-idle-pointer');
    cursor.style.visibility = 'hidden';
  };

  document.addEventListener('pointermove', update, true);
  document.addEventListener('pointerover', update, true);
  document.addEventListener('pointerdown', update, true);
  document.addEventListener('pointerout', event => {
    if (!event.relatedTarget) hide();
  }, true);
  window.addEventListener('blur', hide);
  window.addEventListener('pagehide', hide);
})();
)JS";
        frame->ExecuteJavaScript(
            PersistentCursorScript, frame->GetURL(), 0);

        static constexpr char BrowserUiStateScript[] = R"JS(
(() => {
  const key = 'd2rl-ingame-webbrowser-ui-state-v2';
  const tabs = new Set(['mod', 'uniques', 'sets', 'runewords', 'cube', 'art']);
  const openFilterIds = [
    'unique-slot-filter', 'set-slot-filter', 'runeword-slot-filter',
    'piece-filter-control', 'rune-filter'
  ];
  let session = 'standalone';
  try {
    session = new URLSearchParams(window.location.search).get('d2rl-session') || session;
  } catch {}

  let saved = {
    tab: 'mod', scrollY: 0, search: '', sort: 'default', runes: [],
    setPieces: [], slots: { unique: [], set: [], runeword: [] },
    source: '', sourceSearch: '', sourcePage: 0, openFilters: []
  };
  try {
    const stored = JSON.parse(
      localStorage.getItem(key) || 'null'
    );
    if (stored && stored.session === session) {
      saved = {
        tab: tabs.has(stored.tab) ? stored.tab : 'mod',
        scrollY: Number.isFinite(stored.scrollY) ? Math.max(0, Math.round(stored.scrollY)) : 0,
        search: typeof stored.search === 'string' ? stored.search : '',
        sort: typeof stored.sort === 'string' ? stored.sort : 'default',
        runes: Array.isArray(stored.runes) ? stored.runes.filter(value => typeof value === 'string') : [],
        setPieces: Array.isArray(stored.setPieces) ? stored.setPieces.filter(value => Number.isFinite(value)) : [],
        slots: {
          unique: Array.isArray(stored.slots && stored.slots.unique) ? stored.slots.unique.filter(value => typeof value === 'string') : [],
          set: Array.isArray(stored.slots && stored.slots.set) ? stored.slots.set.filter(value => typeof value === 'string') : [],
          runeword: Array.isArray(stored.slots && stored.slots.runeword) ? stored.slots.runeword.filter(value => typeof value === 'string') : []
        },
        source: typeof stored.source === 'string' ? stored.source : '',
        sourceSearch: typeof stored.sourceSearch === 'string' ? stored.sourceSearch : '',
        sourcePage: Number.isFinite(stored.sourcePage) ? Math.max(0, Math.floor(stored.sourcePage)) : 0,
        openFilters: Array.isArray(stored.openFilters) ? stored.openFilters.filter(value => openFilterIds.includes(value)) : []
      };
    }
  } catch {}

  const activeTab = [...document.querySelectorAll('.tab[data-tab]')]
    .find(button => button.dataset.tab === saved.tab);
  if (activeTab && activeTab.getAttribute('aria-selected') !== 'true') activeTab.click();

  const dispatch = (element, type) => element && element.dispatchEvent(new Event(type, { bubbles: true }));
  const search = document.getElementById('global-search');
  if (search) {
    search.value = saved.search;
    dispatch(search, 'input');
  }

  const sort = document.getElementById('sort-list');
  if (sort && [...sort.options].some(option => option.value === saved.sort)) {
    sort.value = saved.sort;
    dispatch(sort, 'change');
  }

  const restoreChecks = (containerId, values) => {
    const container = document.getElementById(containerId);
    if (!container) return;
    const wanted = new Set(values.map(String));
    const inputs = [...container.querySelectorAll('input[type="checkbox"]')];
    let changed = null;
    inputs.forEach(input => {
      input.checked = wanted.has(input.value);
      if (input.checked) changed = input;
    });
    if (changed) dispatch(changed, 'change');
  };
  restoreChecks('rune-filter-options', saved.runes);
  restoreChecks('piece-filter-options', saved.setPieces);
  restoreChecks('unique-slot-options', saved.slots.unique);
  restoreChecks('set-slot-options', saved.slots.set);
  restoreChecks('runeword-slot-options', saved.slots.runeword);

  const source = document.getElementById('source-select');
  if (source && [...source.options].some(option => option.value === saved.source)) {
    source.value = saved.source;
    dispatch(source, 'change');
  }
  const sourceSearch = document.getElementById('source-search');
  if (sourceSearch) {
    sourceSearch.value = saved.sourceSearch;
    dispatch(sourceSearch, 'input');
  }
  const sourceNext = document.getElementById('source-next');
  if (sourceNext) {
    for (let page = 0; page < saved.sourcePage && !sourceNext.disabled; ++page) sourceNext.click();
  }
  openFilterIds.forEach(id => {
    const filter = document.getElementById(id);
    if (filter) filter.open = saved.openFilters.includes(id);
  });

  const save = () => {
    try {
      const currentTab = document.querySelector('.tab[aria-selected="true"][data-tab]');
      const values = containerId => [...(document.getElementById(containerId)?.querySelectorAll('input[type="checkbox"]:checked') || [])]
        .map(input => input.value);
      const pageMatch = document.getElementById('source-page')?.textContent.match(/Page\s+(\d+)\s+of\s+\d+/i);
      localStorage.setItem(key, JSON.stringify({
        session,
        tab: currentTab && tabs.has(currentTab.dataset.tab) ? currentTab.dataset.tab : 'mod',
        scrollY: Math.max(0, Math.round(window.scrollY || 0)),
        search: document.getElementById('global-search')?.value || '',
        sort: document.getElementById('sort-list')?.value || 'default',
        runes: values('rune-filter-options'),
        setPieces: values('piece-filter-options').map(Number),
        slots: {
          unique: values('unique-slot-options'),
          set: values('set-slot-options'),
          runeword: values('runeword-slot-options')
        },
        source: document.getElementById('source-select')?.value || '',
        sourceSearch: document.getElementById('source-search')?.value || '',
        sourcePage: pageMatch ? Math.max(0, Number(pageMatch[1]) - 1) : 0,
        openFilters: openFilterIds.filter(id => document.getElementById(id)?.open)
      }));
    } catch {}
  };

  document.documentElement.style.scrollBehavior = 'auto';
  window.scrollTo(0, saved.scrollY);
  requestAnimationFrame(() => requestAnimationFrame(() => {
    document.documentElement.style.removeProperty('scroll-behavior');
    save();
  }));

  let saveTimer = 0;
  document.addEventListener('click', event => {
    const target = event.target instanceof Element
      ? event.target.closest('.tab[data-tab]')
      : null;
    if (target || event.target instanceof Element) window.setTimeout(save, 0);
  }, true);
  document.addEventListener('input', () => window.setTimeout(save, 0), true);
  document.addEventListener('change', () => window.setTimeout(save, 0), true);
  document.addEventListener('toggle', () => window.setTimeout(save, 0), true);
  window.addEventListener('scroll', () => {
    window.clearTimeout(saveTimer);
    saveTimer = window.setTimeout(save, 120);
  }, { passive: true });
  window.addEventListener('pagehide', save);
  window.addEventListener('beforeunload', save);
})();
)JS";
        frame->ExecuteJavaScript(BrowserUiStateScript, frame->GetURL(), 0);
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        if (controlWindow_) {
            DestroyWindow(controlWindow_);
            controlWindow_ = nullptr;
            UnregisterClassW(
                BrowserControlWindowClass, GetModuleHandleW(nullptr));
        }
        browser_ = nullptr;
        CefQuitMessageLoop();
    }

    void CloseBrowserFromControlMessage() noexcept {
        if (browser_) {
            browser_->GetHost()->CloseBrowser(true);
        }
    }

private:
    bool CreateControlWindow() noexcept;

    const HWND escapeMessageWindow_;
    HWND controlWindow_{};
    CefRefPtr<CefBrowser> browser_;

    IMPLEMENT_REFCOUNTING(BrowserClient);
};

LRESULT CALLBACK BrowserControlWindowProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) noexcept {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(
            window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* client = reinterpret_cast<BrowserClient*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == CefCloseBrowserMessage && client) {
        client->CloseBrowserFromControlMessage();
        return 0;
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool BrowserClient::CreateControlWindow() noexcept {
    const auto module = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = BrowserControlWindowProc;
    windowClass.hInstance = module;
    windowClass.lpszClassName = BrowserControlWindowClass;
    if (!RegisterClassExW(&windowClass)
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    controlWindow_ = CreateWindowExW(
        0,
        BrowserControlWindowClass,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        module,
        this);
    if (!controlWindow_) {
        UnregisterClassW(BrowserControlWindowClass, module);
        return false;
    }

    if (!escapeMessageWindow_
        || !PostMessageW(
            escapeMessageWindow_,
            BrowserReadyMessage,
            reinterpret_cast<WPARAM>(controlWindow_),
            0)) {
        DestroyWindow(controlWindow_);
        controlWindow_ = nullptr;
        UnregisterClassW(BrowserControlWindowClass, module);
        return false;
    }
    return true;
}

class BrowserApp final : public CefApp, public CefBrowserProcessHandler {
public:
    BrowserApp(
            std::wstring initialUrl,
            HWND parentWindow,
            HWND escapeMessageWindow,
            CefRect bounds)
        : initialUrl_(std::move(initialUrl)),
          parentWindow_(parentWindow),
          escapeMessageWindow_(escapeMessageWindow),
          bounds_(bounds) {}

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

    void OnContextInitialized() override {
        CefWindowInfo windowInfo;
        if (parentWindow_) {
            windowInfo.SetAsPopup(parentWindow_, "In-Game Web Browser");
            windowInfo.style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS
                | WS_VISIBLE;
            windowInfo.ex_style |= WS_EX_TOOLWINDOW;
            windowInfo.bounds.x = bounds_.x;
            windowInfo.bounds.y = bounds_.y;
            windowInfo.bounds.width = bounds_.width;
            windowInfo.bounds.height = bounds_.height;
        } else {
            windowInfo.SetAsPopup(nullptr, "In-Game Web Browser");
        }
        windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

        CefBrowserSettings browserSettings;
        client_ = new BrowserClient(escapeMessageWindow_);
        if (!CefBrowserHost::CreateBrowser(
                windowInfo,
                client_,
                initialUrl_,
                browserSettings,
                nullptr,
                nullptr)) {
            CefQuitMessageLoop();
        }
    }

    IMPLEMENT_REFCOUNTING(BrowserApp);

private:
    const std::wstring initialUrl_;
    const HWND parentWindow_;
    const HWND escapeMessageWindow_;
    const CefRect bounds_;
    CefRefPtr<BrowserClient> client_;
};

bool ParseIntegerSwitch(
        CefRefPtr<CefCommandLine> commandLine,
        const char* name,
        long long minimum,
        long long maximum,
        long long& value) {
    const auto text = commandLine->GetSwitchValue(name).ToWString();
    if (text.empty()) return false;
    wchar_t* end{};
    const auto parsed = std::wcstoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L'\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    value = parsed;
    return true;
}

int RunBrowser(HINSTANCE instance, void* sandboxInfo) {
    WriteStartupLog("RunWinMain entered.");
    CefMainArgs mainArgs(instance);
    const auto subprocessExitCode = CefExecuteProcess(mainArgs, nullptr, sandboxInfo);
    if (subprocessExitCode >= 0) {
        WriteStartupLog("CefExecuteProcess handled a CEF subprocess.");
        return subprocessExitCode;
    }
    WriteStartupLog("CefExecuteProcess returned -1; continuing as browser process.");

    CefRefPtr<CefCommandLine> commandLine = CefCommandLine::CreateCommandLine();
    commandLine->InitFromString(GetCommandLineW());
    const auto urlSwitch = commandLine->GetSwitchValue("url");
    const std::wstring initialUrl = urlSwitch.empty()
        ? L"about:blank"
        : urlSwitch.ToWString();

    HWND parentWindow{};
    HWND escapeMessageWindow{};
    CefRect browserBounds;
    if (commandLine->HasSwitch("parent-hwnd")) {
        long long parentValue{};
        long long x{};
        long long y{};
        long long width{};
        long long height{};
        if (!ParseIntegerSwitch(commandLine, "parent-hwnd", 1,
                (std::numeric_limits<long long>::max)(), parentValue)
            || !ParseIntegerSwitch(commandLine, "x", -100000, 100000, x)
            || !ParseIntegerSwitch(commandLine, "y", -100000, 100000, y)
            || !ParseIntegerSwitch(commandLine, "width", 1, 20000, width)
            || !ParseIntegerSwitch(commandLine, "height", 1, 20000, height)) {
            WriteStartupLog("Launch failed: one or more browser window arguments were invalid.");
            return 1;
        }
        parentWindow = reinterpret_cast<HWND>(
            static_cast<std::uintptr_t>(parentValue));
        if (!IsWindow(parentWindow)) {
            WriteStartupLog("Launch failed: parent HWND is not a valid window.");
            return 1;
        }
        long long escapeWindowValue{};
        if (!ParseIntegerSwitch(commandLine, "escape-hwnd", 1,
                (std::numeric_limits<long long>::max)(), escapeWindowValue)) {
            WriteStartupLog("Launch failed: escape HWND argument was invalid.");
            return 1;
        }
        escapeMessageWindow = reinterpret_cast<HWND>(
            static_cast<std::uintptr_t>(escapeWindowValue));
        if (!IsWindow(escapeMessageWindow)) {
            WriteStartupLog("Launch failed: escape HWND is not a valid window.");
            return 1;
        }
        browserBounds = CefRect(static_cast<int>(x), static_cast<int>(y),
            static_cast<int>(width), static_cast<int>(height));
    }

    CefSettings settings;
    settings.no_sandbox = sandboxInfo == nullptr;
    settings.multi_threaded_message_loop = false;

    const auto executablePath = GetExecutablePath();
    if (!executablePath.empty()) {
        CefString(&settings.browser_subprocess_path) = executablePath;
        const auto slash = executablePath.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const auto resourcesPath = executablePath.substr(0, slash);
            CefString(&settings.cache_path)
                = resourcesPath + L"\\browser-profile";
            CefString(&settings.resources_dir_path) = resourcesPath;
            CefString(&settings.locales_dir_path)
                = resourcesPath + L"\\locales";
            CefString(&settings.log_file)
                = resourcesPath + L"\\cef-debug.log";
        }
    }
    settings.log_severity = LOGSEVERITY_INFO;

    CefRefPtr<BrowserApp> app(new BrowserApp(
        initialUrl, parentWindow, escapeMessageWindow, browserBounds));
    if (!CefInitialize(mainArgs, settings, app.get(), sandboxInfo)) {
        WriteStartupLog("CefInitialize failed.");
        return CefGetExitCode();
    }

    WriteStartupLog("CefInitialize succeeded; starting browser message loop.");
    CefRunMessageLoop();
    CefShutdown();
    return 0;
}

} // namespace

CEF_BOOTSTRAP_EXPORT int RunWinMain(
        HINSTANCE hInstance,
        LPWSTR,
        int,
        void* sandboxInfo,
        cef_version_info_t*) {
    return RunBrowser(hInstance, sandboxInfo);
}
