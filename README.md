<p align="center"><img src="assets/vitoolbox.png" width="96" alt="Vitool Box"></p>

# 🧰 Vitool Box
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

All VitomDev mini apps in one place. 📦  
Browse the apps, read what each one does, and turn on the ones you want: Vitool Box downloads and installs them for you. Turn one off and it's uninstalled. New apps show up on their own.

---

## 🚀 Features

- 🗂️ **App catalog**: every Vitool Box app with its icon, description, screenshots and release notes
- 🔘 **Turn apps on and off**: on downloads and installs the app, off uninstalls it. No setup wizards
- ⬆️ **Updates**: see when a newer version is out and update with one click
- ✨ **New apps appear automatically**, without updating Vitool Box
- 🔍 **Search** and filter by *Installed* or *Updates*
- ▶️ **Open** installed apps straight from their page
- 🛡️ **Safe downloads**: only official VitomDev releases, each checked against its SHA-256 checksum before it runs
- 👀 Notices apps you installed or removed by other means (Settings → Apps)
- 🌙 Clean Windows 11 look that follows your light or dark theme
- 🪶 Small: no .NET or Visual C++ runtime needed

---

## 📦 Install

Download `VitoolBoxSetup.exe` from [Releases](https://github.com/VitomDev/Vitool-Box/releases/latest) and run it.

- Per-user install, no admin rights needed
- Needs the Microsoft Edge WebView2 Runtime, which comes with Windows 11 and up-to-date Windows 10

Uninstalling Vitool Box (Settings → Apps) keeps the apps you installed with it.

---

## 🖱️ Usage

| Action | How |
| --- | --- |
| Install an app | Turn on its switch, or open it and click **Install** |
| Uninstall an app | Turn off its switch, or open it and click **Uninstall** |
| Update an app | Open it and click **Update to x.y.z** (or the *Updates* filter) |
| Open an installed app | Open its page and click **Open** |
| Refresh the catalog | ⟳ button at the top right |

Some apps (like Iris Image) install for all users, so Windows asks for administrator permission when you turn them on.

---

## 🧩 Adding an app to Vitool Box

An app shows up in Vitool Box when its GitHub repository under **VitomDev**:

1. Has the topic **`vitool-box`**
2. Has a **`vitool.json`** file in its root
3. Has at least one **release** with an Inno Setup installer (`.exe`)

`vitool.json`:

```json
{
  "appId": "{B3E5A1C7-2D84-4F6B-9A0E-7C1D5F38E462}",
  "name": "Kodo QR Instant",
  "tagline": "Optional. The repository description is used otherwise",
  "category": "Utilities",
  "icon": "assets/kodo.png",
  "screenshots": ["assets/screenshots/reader.png"],
  "installer": "KodoSetup.exe",
  "exe": "Kodo.exe"
}
```

| Field | Meaning |
| --- | --- |
| `appId` | The installer's Inno Setup `AppId`, with single braces. Used to find the installed app |
| `name`, `tagline`, `category` | What the catalog shows |
| `icon`, `screenshots` | Paths in the repository (or full `https://` URLs) |
| `installer` | Name of the release asset; `*` matches anything (e.g. `App-Setup-*.exe`). Defaults to the first `.exe` with "setup" in its name |
| `exe` | Program started by **Open**, relative to the install folder |

The installer has to support Inno Setup's silent mode (`/VERYSILENT`), which all of them do by default. The app page shows the repository's README and the release notes.

---

## 🔧 Build

Requirements: Visual Studio 2022 or newer with the C++ x64 tools and Inno Setup 6 (`winget install JRSoftware.InnoSetup`).

```powershell
.\build.ps1
```

It downloads the pinned dependencies into `.deps/` (Windows SDK, WebView2 SDK, CMake and Ninja, all verified by hash), builds everything, runs the tests and writes the installer to `dist/VitoolBoxSetup.exe`.

`.\build.ps1 -SkipTests` and `.\build.ps1 -SkipInstaller` skip those steps.

---

## 🏗️ How it works

- `VitoolBox.exe` (C++) hosts a WebView2 window with the UI in `src/ui` (HTML, CSS, JavaScript)
- The UI finds the apps through the GitHub API and caches the catalog for a few hours
- The C++ side downloads installers, checks their SHA-256 (from the release asset's `digest`), runs them silently and reads the installed apps from Windows' list of programs

---

## 📚 Dependencies

- [Microsoft WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2) — the window content, through the WebView2 Runtime built into Windows

See [third-party notices](THIRD-PARTY-NOTICES.md) for licenses.

---

## 🪟 Compatibility

Windows 10 (version 1809 or newer) and Windows 11, 64-bit.

---

## 🧪 Project Status

🚧 In development.  
The first version works; more features are on the way.

---

## 📄 License

[MIT](LICENSE) © VitomDev

---

## 📲 Contact

Feel free to reach out or follow me:

- 🐙 GitHub: [@VitomDev](https://github.com/VitomDev)
- 🐦 Twitter/X: [@VitomDev](https://twitter.com/VitomDev)
- 🎮 Twitch: [@VitomDev](https://www.twitch.tv/VitomDev)
- 💼 LinkedIn: [Víctor Escós](https://www.linkedin.com/in/v%C3%ADctor-esc%C3%B3s-batalla/)
