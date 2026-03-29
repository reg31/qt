#!/bin/zsh
# Qt Dev Kit installer for macOS
# Downloads and installs the latest Qt dev kits into your Qt installation folder.

set -e

GITHUB_BASE="https://github.com/reg31/qt/releases/latest/download"

KITS=(
    "qt-macos-release-dev"
    "qt-ios-release-dev"
)

# ── Detect Qt root ────────────────────────────────────────────────────────────

detect_qt_root() {
    local candidates=(
        "$HOME/Qt"
        "/usr/local/Qt"
        "/opt/Qt"
    )
    for dir in "${candidates[@]}"; do
        if [[ -d "$dir" ]]; then
            echo "$dir"
            return
        fi
    done
    echo ""
}

QT_ROOT=$(detect_qt_root)

if [[ -z "$QT_ROOT" ]]; then
    echo "Qt installation not found in common locations."
    echo -n "Enter your Qt installation path: "
    read QT_ROOT
    if [[ ! -d "$QT_ROOT" ]]; then
        echo "Error: directory does not exist: $QT_ROOT"
        exit 1
    fi
fi

echo "Qt root: $QT_ROOT"

DEV_DIR="$QT_ROOT/dev"
mkdir -p "$DEV_DIR"
echo "Dev folder: $DEV_DIR"
echo ""

# ── Process each kit ─────────────────────────────────────────────────────────

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

for KIT in "${KITS[@]}"; do
    ZIP="${KIT}.zip"
    URL="${GITHUB_BASE}/${ZIP}"

    echo "Checking $KIT ..."

    STATUS=$(curl -s -o /dev/null -w "%{http_code}" -L --head "$URL")
    if [[ "$STATUS" != "200" ]]; then
        echo "  Not found on GitHub (HTTP $STATUS), skipping."
        echo ""
        continue
    fi

    echo "  Removing existing installation..."
    rm -rf "$DEV_DIR/$KIT"

    echo "  Downloading $ZIP ..."
    curl -L --progress-bar "$URL" -o "$TMP_DIR/$ZIP"

    echo "  Extracting..."
    mkdir -p "$DEV_DIR/$KIT"
    unzip -q "$TMP_DIR/$ZIP" -d "$DEV_DIR/$KIT"
    rm "$TMP_DIR/$ZIP"

    echo "  Fixing permissions..."
    xattr -cr "$DEV_DIR/$KIT"
    find "$DEV_DIR/$KIT" -type f -exec chmod +x {} +

    echo "  Installed: $DEV_DIR/$KIT"
    echo ""
done

# ── PATH setup ───────────────────────────────────────────────────────────────

MACOS_BIN="$DEV_DIR/qt-macos-release-dev/bin"
ZPROFILE="$HOME/.zprofile"

if [[ -d "$MACOS_BIN" ]]; then
    if ! grep -qF "$MACOS_BIN" "$ZPROFILE" 2>/dev/null; then
        echo "\nexport PATH=\"$MACOS_BIN:\$PATH\"" >> "$ZPROFILE"
        echo "Added $MACOS_BIN to PATH in $ZPROFILE"
        echo "Restart your terminal or run: source ~/.zprofile"
    else
        echo "PATH already contains $MACOS_BIN"
    fi
fi

# ── QtCreator hint ───────────────────────────────────────────────────────────

echo ""
echo "Done. To register the kits in QtCreator:"
echo "  Preferences → Kits → Qt Versions → Add"
echo "  Point to: $DEV_DIR/qt-macos-release-dev/bin/qmake"
echo "  Then go to Kits and click Auto-detect."
