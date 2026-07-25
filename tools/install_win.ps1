<#
.SYNOPSIS
  Build and install the akaudio VCV Rack plugin on Windows (MSYS2 / MINGW64).

.DESCRIPTION
  One command for the build+install dance, wrapping the gotchas that bit us
  repeatedly (see CLAUDE.md and the memory notes):

    * Builds in the MINGW64 environment (MSYSTEM=MINGW64) via MSYS2 bash, since
      the plugin needs the mingw-w64 GCC toolchain.
    * Passes RACK_USER_DIR explicitly. plugin.mk derives the install dir from
      LOCALAPPDATA, which is NOT exported into MSYS bash -- so a plain
      `make install` silently copies to C:\msys64\Rack2\ where Rack never looks.
      We compute it here (LOCALAPPDATA is reliable in PowerShell) and pass it in.
    * Refuses to install while Rack is running -- Windows locks the loaded
      plugin.dll, so the copy would fail or leave a half-written DLL.
    * Sanity-checks that plugin.dll exports Rack's `init` symbol before
      installing. Vendored FAAD2's __declspec(dllexport) once silently dropped
      `init` from the export table and Rack refused to load the plugin
      ("Failed to read init() symbol"); this catches any regression of that.

  ASCII-only on purpose: Windows PowerShell 5.1 reads .ps1 as ANSI, so any
  non-ASCII character (e.g. an em dash) corrupts parsing. Keep it that way.

.PARAMETER BuildOnly
  Build and verify, but do not install.

.PARAMETER Jobs
  Parallel make jobs. Defaults to the CPU count.

.PARAMETER Bash
  Path to MSYS2 bash. Defaults to C:\msys64\usr\bin\bash.exe.

.EXAMPLE
  tools\install_win.ps1
      Incremental build, verify, and install into the real Rack folder.

.EXAMPLE
  tools\install_win.ps1 -BuildOnly
      Just build and check the export table (no install; safe while Rack is open).
#>
[CmdletBinding()]
param(
    [switch]$BuildOnly,
    [int]$Jobs = [int]$env:NUMBER_OF_PROCESSORS,
    [string]$Bash = 'C:\msys64\usr\bin\bash.exe'
)

$ErrorActionPreference = 'Stop'
if (-not $Jobs -or $Jobs -lt 1) { $Jobs = 4 }

# --- locate the repo (this script lives in <repo>/tools) and the toolchain -----
$RepoRoot = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path $Bash)) {
    throw "MSYS2 bash not found at '$Bash'. Install it (winget install MSYS2.MSYS2) or pass -Bash <path>."
}

# Rack's per-user dir holds plugins-win-x64\. LOCALAPPDATA is reliable in
# PowerShell (it is not, inside MSYS bash -- that's why we compute+pass it).
$RackUserDir = Join-Path $env:LOCALAPPDATA 'Rack2'

# Windows path -> MSYS/POSIX path (C:\a\b -> /c/a/b), done here so we don't depend
# on cygpath or on how bash sees the drive.
function ConvertTo-Posix([string]$p) {
    $p = $p -replace '\\', '/'
    if ($p -match '^([A-Za-z]):(.*)$') { return '/' + $Matches[1].ToLower() + $Matches[2] }
    return $p
}
$posixRepo = ConvertTo-Posix $RepoRoot
$posixRUD  = ConvertTo-Posix $RackUserDir

# --- guard: Rack must be closed before we overwrite its plugin DLL -------------
if (-not $BuildOnly) {
    $rack = Get-Process Rack -ErrorAction SilentlyContinue
    if ($rack) {
        throw "Rack is running (PID $($rack.Id)). Close it first: the plugin DLL is locked while Rack is open."
    }
}

# --- build (+ verify + install), all inside one MINGW64 login shell ------------
# &&-chained so any failure (compile error, missing init export) aborts before
# install. The init check is the load-time canary for the FAAD2 export trap.
$verify = "objdump -p plugin.dll | grep -qw init || " +
          "{ echo 'ERROR: plugin.dll does not export init(); Rack would refuse to load it.' >&2; exit 1; }"

$steps = @(
    "cd '$posixRepo'",
    "echo '== building (MINGW64, -j$Jobs) =='",
    "make -j$Jobs",
    "echo '== verifying plugin.dll exports init() =='",
    $verify,
    "echo 'ok: init is exported'"
)
if (-not $BuildOnly) {
    $steps += "echo '== installing into $posixRUD =='"
    $steps += "make install RACK_USER_DIR='$posixRUD' -j$Jobs"
}
$bashCmd = $steps -join ' && '

$env:MSYSTEM = 'MINGW64'
& $Bash -lc $bashCmd
if ($LASTEXITCODE -ne 0) { throw "Build/install failed (exit $LASTEXITCODE)." }

# --- report -------------------------------------------------------------------
if ($BuildOnly) {
    Write-Host "`nBuild OK (not installed: -BuildOnly)." -ForegroundColor Green
}
else {
    $pkg = Get-ChildItem "$RackUserDir\plugins-win-x64\akaudio-*.vcvplugin" -ErrorAction SilentlyContinue |
           Select-Object -Last 1
    Write-Host "`nInstalled into $RackUserDir\plugins-win-x64\" -ForegroundColor Green
    if ($pkg) { Write-Host ("  {0}  ({1:N0} bytes)" -f $pkg.Name, $pkg.Length) }
    Write-Host "Launch Rack to load it." -ForegroundColor Green
}
