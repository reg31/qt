# qt

Daily build of QT dev branch
https://github.com/qt/qt5

The purpose is to compile with the latest commit in QtBase and QtDeclarative to test the new features and patches.

Of course, do **not** use in production.

---

## 🚀 Installation

### Option 1 — Install script (recommended)

Run the script for your platform. It will automatically detect your Qt installation, create the `dev` subfolder if needed, download the latest kits, and add them to your PATH.

**macOS:**
```zsh
/bin/zsh -c "$(curl -fsSL https://raw.githubusercontent.com/reg31/qt/main/files/scripts/install.sh)"
```

**Windows** (run in PowerShell as administrator):
```powershell
powershell -ExecutionPolicy Bypass -Command "Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/reg31/qt/main/files/scripts/install.ps1' -OutFile install.ps1; ./install.ps1"
```

Re-running the script at any time will update the kits to the latest build.

### Option 2 — Manual installation

Download the zip files from the Releases section and extract them into a `dev` subfolder inside your Qt installation folder. Each kit must be in its own subfolder matching the zip name.

* `qt-windows-mingw-release-dev.zip` → `<Qt>/dev/qt-windows-mingw-release-dev/`
* `qt-android-arm64-v8a-release-dev.zip` → `<Qt>/dev/qt-android-arm64-v8a-release-dev/`
* `qt-android-armeabi-v7a-release-dev.zip` → `<Qt>/dev/qt-android-armeabi-v7a-release-dev/`
* `qt-macos-release-dev.zip` → `<Qt>/dev/qt-macos-release-dev/`
* `qt-ios-release-dev.zip` → `<Qt>/dev/qt-ios-release-dev/`

#### macOS & iOS — fix permissions after unzipping

After extracting, you **must** clear the quarantine attribute and set executable permissions. Run the following in the terminal from the unzipped folder:

```zsh
xattr -cr .
find . -type f -exec chmod +x {} +
```

Skipping this step may result in binaries being blocked by Gatekeeper or failing to execute.

---

## 🔧 Registering kits in QtCreator

After installation, add the Qt version manually once:

**Preferences → Kits → Qt Versions → Add**

Point to the `qmake` binary inside the installed kit:
* macOS: `<Qt>/dev/qt-macos-release-dev/bin/qmake`
* Windows: `<Qt>\dev\qt-windows-mingw-release-dev\bin\qmake.exe`

Then go to **Kits** and click **Auto-detect**.
