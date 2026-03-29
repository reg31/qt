# Qt Dev Kit installer for Windows
# Downloads and installs the latest Qt dev kits into your Qt installation folder.
# Run with: powershell -ExecutionPolicy Bypass -File install.ps1

$ErrorActionPreference = "Stop"

$GithubBase = "https://github.com/reg31/qt/releases/latest/download"

$Kits = @(
    "qt-windows-mingw-release-dev",
    "qt-android-arm64-v8a-release-dev",
    "qt-android-armeabi-v7a-release-dev"
)

# ── Detect Qt root ────────────────────────────────────────────────────────────

function Find-QtRoot {
    # Try registry first
    $regPaths = @(
        "HKLM:\SOFTWARE\Qt",
        "HKLM:\SOFTWARE\WOW6432Node\Qt"
    )
    foreach ($reg in $regPaths) {
        if (Test-Path $reg) {
            $val = (Get-ItemProperty $reg -ErrorAction SilentlyContinue)."Install_Dir"
            if ($val -and (Test-Path $val)) { return $val }
        }
    }
    # Fallback: common locations
    $candidates = @("C:\Qt", "D:\Qt")
    foreach ($dir in $candidates) {
        if (Test-Path $dir) { return $dir }
    }
    return $null
}

$QtRoot = Find-QtRoot

if (-not $QtRoot) {
    Write-Host "Qt installation not found in common locations."
    $QtRoot = Read-Host "Enter your Qt installation path"
    if (-not (Test-Path $QtRoot)) {
        Write-Error "Directory does not exist: $QtRoot"
        exit 1
    }
}

Write-Host "Qt root: $QtRoot"

$DevDir = Join-Path $QtRoot "dev"
if (-not (Test-Path $DevDir)) {
    New-Item -ItemType Directory -Path $DevDir | Out-Null
}
Write-Host "Dev folder: $DevDir"
Write-Host ""

# ── Process each kit ─────────────────────────────────────────────────────────

$TmpDir = Join-Path $env:TEMP "qt-dev-install"
New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

foreach ($Kit in $Kits) {
    $Zip = "$Kit.zip"
    $Url = "$GithubBase/$Zip"

    Write-Host "Checking $Kit ..."

    # Check if archive exists on GitHub
    try {
        $Response = Invoke-WebRequest -Uri $Url -Method Head -UseBasicParsing -ErrorAction Stop
        $Status = $Response.StatusCode
    } catch {
        $Status = $_.Exception.Response.StatusCode.value__
    }

    if ($Status -ne 200) {
        Write-Host "  Not found on GitHub (HTTP $Status), skipping."
        Write-Host ""
        continue
    }

    # Remove existing subfolder if present
    $KitDir = Join-Path $DevDir $Kit
    Write-Host "  Removing existing installation..."
    Remove-Item -Recurse -Force $KitDir -ErrorAction SilentlyContinue

    # Download
    $ZipPath = Join-Path $TmpDir $Zip
    Write-Host "  Downloading $Zip ..."
    Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing

    # Extract
    Write-Host "  Extracting..."
    New-Item -ItemType Directory -Force -Path $KitDir | Out-Null
    Expand-Archive -Path $ZipPath -DestinationPath $KitDir -Force
    Remove-Item $ZipPath

    Write-Host "  Installed: $KitDir"
    Write-Host ""
}

Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue

# ── PATH setup ───────────────────────────────────────────────────────────────

$BinPath = Join-Path $DevDir "qt-windows-mingw-release-dev\bin"

if (Test-Path $BinPath) {
    $CurrentPath = [Environment]::GetEnvironmentVariable("PATH", "User")
    if ($CurrentPath -notlike "*$BinPath*") {
        [Environment]::SetEnvironmentVariable("PATH", "$CurrentPath;$BinPath", "User")
        Write-Host "Added $BinPath to user PATH."
        Write-Host "Restart your terminal for the change to take effect."
    } else {
        Write-Host "PATH already contains $BinPath"
    }
}

# ── QtCreator hint ───────────────────────────────────────────────────────────

Write-Host ""
Write-Host "Done. To register the kits in QtCreator:"
Write-Host "  Tools -> Options -> Kits -> Qt Versions -> Add"
Write-Host "  Point to: $DevDir\qt-windows-mingw-release-dev\bin\qmake.exe"
Write-Host "  Then go to Kits and click Auto-detect."
