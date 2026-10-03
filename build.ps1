<#
.SYNOPSIS
  Builds sk.exe into .\build and copies the DLLs it needs next to it (only if it needs any).

.EXAMPLE
  .\build.ps1                          # finds libcurl on its own
  .\build.ps1 -Curl C:\curl\curl-8.x-win64-mingw
  .\build.ps1 -Static                  # single sk.exe, no DLLs (needs a static libcurl)
  .\build.ps1 -Clean                   # delete .\build

.NOTES
  -Curl  folder that contains include\curl\curl.h, lib\ and bin\
         (curl.se/windows "win64-mingw" zip, or C:\msys64\mingw64)
  -Cc    compiler, default gcc from PATH
  -Extra extra linker flags, e.g. for -Static:  -Extra '-lws2_32 -lcrypt32 ...'
         (if omitted, -Static reads them from <Curl>\lib\pkgconfig\libcurl.pc)
#>
param(
    [string]$Curl = $env:CURL,
    [string]$Cc = "gcc",
    [switch]$Static,
    [string]$Extra = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$Root  = $PSScriptRoot
$Build = Join-Path $Root "build"
$Exe   = Join-Path $Build "sk.exe"

if ($Clean) {
    if (Test-Path $Build) { Remove-Item -Recurse -Force $Build }
    Write-Host "[+] removed build\"
    return
}

# ---- find libcurl ----------------------------------------------------------

function Test-CurlDir([string]$Dir) {
    $Dir -and (Test-Path (Join-Path $Dir "include/curl/curl.h"))
}

function Find-CurlDir {
    $candidates = @()
    foreach ($base in @("C:\curl", "C:\msys64\mingw64", $env:MSYSTEM_PREFIX)) {
        if (-not $base) { continue }
        $candidates += $base
        if (Test-Path $base) {
            $candidates += (Get-ChildItem $base -Directory -ErrorAction SilentlyContinue).FullName
        }
    }
    if (Test-Path "C:\") {
        $candidates += (Get-ChildItem "C:\" -Directory -Filter "curl*" -ErrorAction SilentlyContinue).FullName
    }
    foreach ($c in $candidates) { if (Test-CurlDir $c) { return $c } }
    return $null
}

if (-not $Curl) { $Curl = Find-CurlDir }
if (-not (Test-CurlDir $Curl)) {
    Write-Host "[-] libcurl not found." -ForegroundColor Red
    Write-Host "    Pass the folder that holds include\curl\curl.h:  .\build.ps1 -Curl C:\path\to\curl"
    Write-Host "    (curl.se/windows 'win64-mingw' zip, or C:\msys64\mingw64 after: pacman -S mingw-w64-x86_64-curl)"
    exit 1
}
if (-not (Get-Command $Cc -ErrorAction SilentlyContinue)) {
    Write-Host "[-] compiler '$Cc' not found on PATH." -ForegroundColor Red
    exit 1
}
$target = (& $Cc -dumpmachine 2>$null)
if ($target -notmatch 'mingw') {
    Write-Host "[-] '$Cc' builds for '$target', not MinGW." -ForegroundColor Red
    Write-Host "    Use Chocolatey's mingw or MSYS2's mingw64 gcc (not the plain MSYS one)."
    exit 1
}
Write-Host "[*] libcurl: $Curl"

# ---- compile ----------------------------------------------------------------

New-Item -ItemType Directory -Force -Path $Build | Out-Null
if (Test-Path $Exe) { Remove-Item -Force $Exe }

$sources = "sk.c", "net.c", "inst.c", "extract.c", "cjson.c" | ForEach-Object { Join-Path $Root $_ }
$cflags  = @("-O2", "-Wall", "-Wextra", "-Wno-format-truncation", "-I$(Join-Path $Curl 'include')")
$ldflags = @("-L$(Join-Path $Curl 'lib')")
$libs    = @("-lcurl")

if ($Static) {
    $cflags += "-DCURL_STATICLIB"
    if ($Extra) {
        $libs += $Extra -split '\s+' | Where-Object { $_ }
    }
    elseif ((Get-Command pkg-config -ErrorAction SilentlyContinue) -and
            (Test-Path (Join-Path $Curl "lib/pkgconfig/libcurl.pc"))) {
        # only ask pkg-config about THIS libcurl, never one it finds elsewhere on the system
        $oldPc = $env:PKG_CONFIG_PATH
        $env:PKG_CONFIG_PATH = Join-Path $Curl "lib/pkgconfig"
        try { $libs += (& pkg-config --static --libs libcurl) -split '\s+' | Where-Object { $_ } }
        finally { $env:PKG_CONFIG_PATH = $oldPc }
    }
    else {
        Write-Warning "-Static: no -Extra given and no libcurl.pc under $Curl\lib\pkgconfig, so the link may miss libraries."
    }
}
elseif ($Extra) {
    $libs += $Extra -split '\s+' | Where-Object { $_ }
}
$libs += "-lshlwapi", "-lole32", "-lshell32", "-luuid"

Write-Host "[*] compiling..."
& $Cc @cflags -o $Exe @sources @ldflags @libs
if ($LASTEXITCODE -ne 0) {
    Write-Host "[-] build failed." -ForegroundColor Red
    exit 1
}
Write-Host "[+] $Exe" -ForegroundColor Green

# ---- copy the DLLs sk.exe needs, and only those ------------------------------

$objdump = $Cc -replace 'gcc(\.exe)?$', 'objdump'
$searchDirs = @(
    (Join-Path $Curl "bin"),
    (Join-Path $Curl "lib"),
    (Split-Path (Get-Command $Cc).Source)
) | Where-Object { Test-Path $_ }

# DLLs Windows itself provides; anything else imported has to ship with sk.exe
$windowsDlls = @(
    "kernel32", "kernelbase", "ntdll", "user32", "gdi32", "advapi32", "shell32", "shlwapi", "ole32",
    "oleaut32", "ws2_32", "crypt32", "bcrypt", "wldap32", "normaliz", "iphlpapi", "secur32", "msvcrt",
    "ucrtbase", "comctl32", "comdlg32", "rpcrt4", "sechost", "imm32", "version"
)

function Test-WindowsDll([string]$Name) {
    $n = $Name.ToLower()
    if ($n -like "api-ms-win-*") { return $true }
    if ($windowsDlls -contains ($n -replace '\.dll$', '')) { return $true }
    if ($env:SystemRoot) {
        foreach ($d in "System32", "SysWOW64") {
            if (Test-Path (Join-Path (Join-Path $env:SystemRoot $d) $Name)) { return $true }
        }
    }
    return $false
}

function Get-Imports([string]$File) {
    & $objdump -p $File 2>$null |
        Select-String -Pattern 'DLL Name:\s*(\S+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
}

if ($Static) {
    Write-Host "[+] static build: no DLLs needed" -ForegroundColor Green
}
elseif (-not (Get-Command $objdump -ErrorAction SilentlyContinue)) {
    Write-Warning "$objdump not found; copying every DLL from $(Join-Path $Curl 'bin') instead."
    Get-ChildItem (Join-Path $Curl "bin") -Filter *.dll -ErrorAction SilentlyContinue |
        Copy-Item -Destination $Build -Force
}
else {
    $queue  = [System.Collections.Generic.Queue[string]]::new()
    $seen   = @{}
    $copied = @()
    $queue.Enqueue($Exe)
    while ($queue.Count -gt 0) {
        $file = $queue.Dequeue()
        foreach ($dll in Get-Imports $file) {
            $key = $dll.ToLower()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            if (Test-WindowsDll $dll) { continue }

            $src = $searchDirs | ForEach-Object { Join-Path $_ $dll } | Where-Object { Test-Path $_ } | Select-Object -First 1
            if (-not $src) {
                Write-Warning "$dll is needed but was not found in: $($searchDirs -join ', ')"
                continue
            }
            $dst = Join-Path $Build $dll
            Copy-Item $src $dst -Force
            $copied += $dll
            $queue.Enqueue($dst)   # its own dependencies come next
        }
    }
    if ($copied.Count -eq 0) { Write-Host "[+] no extra DLLs needed" -ForegroundColor Green }
    else { Write-Host "[+] copied DLLs: $($copied -join ', ')" -ForegroundColor Green }
}

Write-Host ""
Get-ChildItem $Build | ForEach-Object { "    {0,10:N0}  {1}" -f $_.Length, $_.Name }
