# Hook Inventory — Webbrowser In-Game Plugin

## Plugin metadata

| Field | Value |
|---|---|
| Plugin name | Webbrowser In-Game Plugin |
| Plugin ID | d2rl-ingame-webbrowser |
| Release version | 0.2.36 |
| Inventory updated | 2026-11-08 |
| Source revision | v0.2.36 release source |
| D2R build(s) audited | Not validated |
| D2RLoader SDK / Plugin ABI | PluginSDK 0.4.0 / Plugin ABI 4 |
| Audit status | Complete |

## Scope and method

Inspected `src/plugin.cpp`, `src/cef_client.cpp`, `src/plugin.rc`, and `CMakeLists.txt` for plugin-owned native or operating-system hook installation and patching. The only direct hook site is a thread-scoped Win32 keyboard hook used while the browser panel is open. Ordinary D2RLoader panel, resource, and shared-event service calls are not hook sites.

## Hook sites

| Site ID | Type | Target | Address / identifier | Operation | Guard / validation | Source reference | Notes |
|---|---|---|---|---|---|---|---|
| H-01 | Win32 input-message | D2RLoader game-host window thread, `WH_KEYBOARD` | N/A | `SetWindowsHookExW` installs `BrowserKeyboardHookProc`; `UnhookWindowsHookEx` removes it | Requires a valid game-host window belonging to the current process and a resolved window thread; only active while CEF is open and removed when the browser closes | `src/plugin.cpp` — `InstallBrowserEscapeHook`, `BrowserKeyboardHookProc`, `RemoveBrowserEscapeHook` | Filters Escape so the in-game panel closes before the pause menu. |

## Known collision overlaps

| Other plugin | Shared target | Relationship | Evidence / notes |
|---|---|---|---|
| Shared Stash Page Text | `WH_KEYBOARD` on the D2RLoader game-host thread | Possible shared thread keyboard-hook chain | Both source snapshots install thread-scoped keyboard hooks for UI input. |

## Audit notes

No native executable hook, patch, vtable replacement, or shared native-slot replacement was found. The hook site is unchanged from v0.2.35. Source audit is complete; in-game validation of v0.2.36 was not performed.
