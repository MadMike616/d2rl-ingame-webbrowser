# Webbrowser In-Game Plugin

A D2RLoader plugin that displays a local HTML page in an in-game CEF panel. Mod creators can use it to show a guide, database, or other local web content inside their mod.

The panel uses a native D2R frame and close button. Players can close it with **Esc** or the close button. An open-in-browser control launches the configured page in the computer's default browser and closes the in-game panel.

The package includes `example.html` as sample content. It is optional: use it as a starting point, replace it with your own page, or set `html_file` to another local HTML file.

## Requirements

- D2RLoader with Plugin ABI 4 and the panel, resource, and shared-event services.
- PluginSDK 0.4.0 headers (vendored in this repository).
- The matching CEF 152.0.14 Windows x64 minimal SDK to build, and its complete runtime in the release package.
- A local HTML page to display.

## Installation

Install the plugin in the target mod's D2RLoader plugin folder:

```text
mods\<YourMod>\d2rloader\plugins\
```

Place the plugin DLL, the HTML page selected by `html_file`, and the complete `CEF corefiles` folder together:

```text
plugins\
├── d2rl-ingame-webbrowser.dll
├── <your-page>.html
└── CEF corefiles\
    ├── cefsimple.exe
    ├── cefsimple.dll
    ├── libcef.dll
    ├── other matching CEF runtime files
    └── locales\
```

If you use the included demonstration page, keep it beside the DLL. The plugin does not create or download the CEF runtime. Keep all runtime files from one matching CEF build together in `CEF corefiles`; they do not go beside `D2RLoader.exe`.

## Connect a button in your mod

The plugin registers its panel as `d2rl-ingame-webbrowser/BrowserPanel`. Add this click message to a button in your mod's layout to toggle the panel:

```json
"onClickMessage": "PanelManager:TogglePanel:d2rl-ingame-webbrowser/BrowserPanel"
```

Choose the button location, sprite, label, and tooltip to match your mod. On first load, the plugin creates `BrowserPanelhd.json` beside its DLL. Edit that file to customize the panel title and layout; the plugin preserves it during updates.

## Configure the HTML page

D2RLoader creates `d2rl-ingame-webbrowser.toml` in the target mod's `d2rloader\config` folder. Set `html_file` to the page you want to display:

```toml
html_file = "example.html"
```

Relative paths resolve from the folder containing the plugin DLL. Absolute Windows paths can use a TOML literal string to preserve backslashes:

```toml
html_file = 'E:\mods\MyMod\mod-guide.html'
```

## Build

The build requires CMake 3.29 or newer, a Visual Studio x64 Developer Command Prompt, and the vendored D2RLoader PluginSDK 0.4.0 headers with Plugin ABI 4.

Download and extract the matching CEF 152.0.14 Windows x64 minimal package from the [CEF builds page](https://cef-builds.spotifycdn.com/index.html). The extracted directory must contain `CMakeLists.txt`, `include`, `Release`, and `Resources`. From the plugin source folder, build outside that folder:

```powershell
cmake -S . -B ..\build\webbrowser-ingame -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release '-DCEF_ROOT=C:\deps\cef-sdk-152'
cmake --build ..\build\webbrowser-ingame --target stage_all
```

The installable files are staged in:

```text
..\build\webbrowser-ingame\stage\plugin\
```

Copy the plugin DLL, the HTML page configured by `html_file`, and the entire `CEF corefiles` directory into the target mod's plugin folder.

## Licenses

The plugin source is licensed under the MIT License. Third-party licenses and notices are included with the corresponding packages and summarized in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
