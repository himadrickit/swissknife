# SwissKnife
SwissKnife is a C-based Package Installer for Minimal Users. It uses Git Repo so you can make your own sorce any way.
[Will be a full-fletched Package Manager]

## Installation
You can install [wheat](https://github.com/HimadriChakra12/wheat) and install it from there.
Or,
```powershell
iwr -useb "https://tinyurl.com/hswiss" | iex 
```
Copy and paste the code to Powershell. Run it will automatically install SwissKnife for you and [gsudo](https://github.com/gerardog/gsudo#installation) gets installed with it. Using `sudo` for admin previlages is good.

## [Sa]usage
It has a pacman type option to install with a alias of `sk`

```
  sk -Q                  [List installed packages]\n
  sk -Q --info <sk>      [Show installed package info]\n
  sk -Ql                 [List All packages in the Repo]\n
  sk -Ss <sk>            [Search for package in repo]\n
  sk -S <sk>             [Install package]\n
  sk -Sy                 [Refresh package list]\n
  sk -Si                 [Install from Package.json]\n
  sk -Su                 [Check for updates]\n
  sk -Sr <url>           [Set repo URL]\n
```
# Compile
```
.\build.ps1                      # finds libcurl, builds build\sk.exe, copies the DLLs it needs
.\build.ps1 -Curl C:\curl\curl-8.x-win64-mingw
.\build.ps1 -Static              # one sk.exe, no DLLs (needs a static libcurl)
```
Needs a MinGW build of libcurl: the "win64-mingw" zip from curl.se/windows, or MSYS2's `mingw-w64-x86_64-curl`. `-Curl` is the folder holding `include\`, `lib\`, `bin\`.
`mingw32-make CURL=C:/path/to/libcurl` also works and writes `sk.exe` in the source folder.

# Runtime needs
`git` on PATH (knives), `tar.exe` (ships with Windows 10+) for zip/tar/gz/bz2/xz, and 7-Zip for `.7z` (falls back to tar if missing). Quiet by default; pass `--nosilent` to let installers show their UI. Tweak switches, markers, tools and timeouts in `config.h`.
