# qt

Daily build of QT dev branch
https://github.com/qt/qt5

The purpose is to compile with the latest commit in QtBase and QtDeclarative to test the new features and patches.

Of course, do **not** use in production.

---

## 📦 Downloads

The latest prebuilt kits are available in the Releases section.

Each zip file must be extracted **as-is**, keeping the folder name matching the zip name. Do not rename the extracted folder.

* Unzip `qt-android-arm64-v8a-release-dev.zip` — extracts to `qt-android-arm64-v8a-release-dev/`
* Unzip `qt-android-armeabi-v7a-release-dev.zip` — extracts to `qt-android-armeabi-v7a-release-dev/`
* Unzip `qt-ios-release-dev.zip` — extracts to `qt-ios-release-dev/`
* Unzip `qt-macos-release-dev.zip` — extracts to `qt-macos-release-dev/`
* Unzip `qt-windows-mingw-release-dev.zip` — extracts to `qt-windows-mingw-release-dev/`

---

## ⚠️ macOS & iOS — Fix Permissions After Unzipping

After extracting the kit, you **must** run the following script from the root of the unzipped folder to set the correct file permissions and remove quarantine attributes. This can be done by either running the script directly or by typing the commands manually in the terminal.

```zsh
#!/bin/zsh
cd "$(dirname "$0")"
xattr -cr .
find . -type f -exec chmod +x {} +
```

**Option 1 — Run the script:**
```sh
chmod +x fix_permissions.sh
./fix_permissions.sh
```

**Option 2 — Run directly in the terminal:** open a terminal, navigate to the unzipped folder, and run:
```sh
xattr -cr .
find . -type f -exec chmod +x {} +
```

Skipping this step may result in binaries being blocked by Gatekeeper or failing to execute.
