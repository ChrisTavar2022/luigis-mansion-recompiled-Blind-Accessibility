# One-step release: build LuigisMansionRecomp, package it as
# out/LuigisMansionPC-<version>-windows-x64.zip, build the Linux packages in WSL
# (tools/release_linux.sh; skipped with -SkipLinux or when WSL is not installed), then publish
# GitHub release v<version> with every package and packaging/release_notes/v<version>.md
# (download table filled in).
# Publishes with the GitHub CLI (gh) when it is installed and logged in; otherwise opens
# the "new release" page in the browser, filled in, and the zip's folder in Explorer.
#   powershell -ExecutionPolicy Bypass -File tools\release_windows.ps1 [-Preset windows-clang]
# (tools\release_windows.bat does the same on a double-click.)
param(
    [string]$Preset = "windows-clang",
    [switch]$SkipLinux
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$repo = "rob646464/luigis-mansion-pc-port"

function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }

Push-Location $root
try {
    # Version from CMakeLists.txt: project(lmport ... VERSION x.y.z)
    $m = Select-String -Path CMakeLists.txt -Pattern 'project\(lmport .*VERSION ([0-9.]+)\)' | Select-Object -First 1
    if (-not $m) { throw "project version not found in CMakeLists.txt" }
    $version = $m.Matches[0].Groups[1].Value
    $tag = "v$version"
    $notesSrc = "packaging/release_notes/$tag.md"
    if (-not (Test-Path $notesSrc)) { throw "release notes $notesSrc missing" }
    Step "Releasing Luigi's Mansion PC $version ($tag)"

    $branch = (git rev-parse --abbrev-ref HEAD).Trim()
    if ($branch -ne "main") { Write-Host "Warning: building branch '$branch', not main." -ForegroundColor Yellow }

    Step "Building (this can take a while)"
    & powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\build.ps1" -Preset $Preset -Target LuigisMansionRecomp
    if ($LASTEXITCODE) { throw "build failed" }

    foreach ($p in @("$env:ProgramFiles\CMake\bin", "$env:LOCALAPPDATA\Microsoft\WinGet\Links")) {
        if (Test-Path $p) { $env:PATH = "$p;$env:PATH" }
    }

    Step "Packaging"
    $name = "LuigisMansionPC-$version-windows-x64"
    $outDir = Join-Path $root "out"
    $stage = Join-Path $outDir $name
    $zip = Join-Path $outDir "$name.zip"
    New-Item -ItemType Directory -Force $outDir | Out-Null
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    if (Test-Path $zip) { Remove-Item -Force $zip }
    cmake --install "build/$Preset" --prefix $stage
    if ($LASTEXITCODE) { throw "cmake --install failed" }
    foreach ($f in @("LuigisMansionRecomp.exe", "pipelines.bin", "llvm\bin\clang.exe", "llvm\bin\lld-link.exe")) {
        if (-not (Test-Path (Join-Path $stage $f))) { throw "package is missing $f" }
    }
    Compress-Archive -Path $stage -DestinationPath $zip
    $assets = @(@{ File = $zip; Platform = "Windows 10/11, 64-bit, Direct3D 12" })

    $linuxNote = ""
    if ($SkipLinux) {
        $linuxNote = "Linux: no $version package; the previous Linux package still works.`n"
    } elseif (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        Write-Host "WSL is not installed: no Linux package (install it with 'wsl --install')." -ForegroundColor Yellow
        $linuxNote = "Linux: no $version package; the previous Linux package still works.`n"
    } else {
        Step "Building the Linux packages in WSL (asks for your Linux password if packages are missing)"
        & wsl.exe --cd "$root" bash tools/release_linux.sh $version out
        $tgz = Join-Path $outDir "LuigisMansionPC-$version-linux-x86_64.tar.gz"
        if ($LASTEXITCODE -or -not (Test-Path $tgz)) {
            Write-Host ""
            Write-Host "The Linux build failed (messages above)." -ForegroundColor Red
            $answer = Read-Host "Publish the Windows package alone? (y/n)"
            if ($answer -notmatch '^[yY]') { throw "stopped: Linux build failed" }
            $linuxNote = "Linux: no $version package; the previous Linux package still works.`n"
        } else {
            # Only the .tar.gz is published (it keeps file permissions); the .zip release_linux.sh
            # also writes stays in out/.
            $assets += @{ File = $tgz; Platform = "Linux x86_64, glibc 2.34+, Vulkan" }
        }
    }

    $rows = foreach ($a in $assets) {
        $h = (Get-FileHash -Algorithm SHA256 $a.File).Hash.ToLower()
        $leaf = Split-Path -Leaf $a.File
        Write-Host "$leaf  $([math]::Round((Get-Item $a.File).Length / 1MB)) MB  sha256 $h"
        "| ``$leaf`` | $($a.Platform) | ``$h`` |"
    }
    $notes = (Get-Content -Raw -Encoding UTF8 $notesSrc).Replace("{{DOWNLOAD_ROWS}}", ($rows -join "`n")).Replace("{{LINUX_NOTE}}", $linuxNote)
    $notesFile = Join-Path $outDir "$tag-notes.md"
    [IO.File]::WriteAllText($notesFile, $notes, (New-Object Text.UTF8Encoding $false))
    $title = "Luigi's Mansion PC $version"

    Step "Publishing GitHub release $tag"
    $gh = Get-Command gh -ErrorAction SilentlyContinue
    $ghOk = $false
    if ($gh) {
        # gh prints its status to stderr, which Windows PowerShell turns into an error record
        # (fatal under ErrorActionPreference=Stop): only the exit code matters here.
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & gh auth status *> $null
        $ghOk = ($LASTEXITCODE -eq 0)
        $ErrorActionPreference = $prev
    }
    if ($ghOk) {
        & gh release create $tag @($assets | ForEach-Object { $_.File }) --repo $repo --target main --title $title --notes-file $notesFile
        if ($LASTEXITCODE) { throw "gh release create failed" }
        Write-Host ""
        Write-Host "Done: https://github.com/$repo/releases/tag/$tag" -ForegroundColor Green
    } else {
        $url = "https://github.com/$repo/releases/new?tag=$tag&target=main" +
               "&title=" + [uri]::EscapeDataString($title) + "&body=" + [uri]::EscapeDataString($notes)
        Start-Process $url
        Start-Process explorer.exe "`"$outDir`""
        Write-Host ""
        Write-Host "The GitHub page for the new release is open in your browser, already filled in." -ForegroundColor Green
        Write-Host "1. From the Explorer window (the 'out' folder), drag these files onto the 'Attach binaries' box:"
        foreach ($a in $assets) { Write-Host "     $(Split-Path -Leaf $a.File)" }
        Write-Host "2. Wait for every upload to finish, then click 'Publish release'."
    }
} finally {
    Pop-Location
}
