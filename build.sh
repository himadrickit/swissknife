#!/usr/bin/env bash

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
ZIP="$ROOT/sk.zip"

# ---- find MinGW GCC --------------------------------------------------------

if [[ -n "$MINGW_PREFIX" && -x "$MINGW_PREFIX/bin/gcc.exe" ]]; then
    PREFIX="$MINGW_PREFIX"
    CC="$MINGW_PREFIX/bin/gcc.exe"
else
    for prefix in \
        /ucrt64 \
        /mingw64 \
        /clang64 \
        /clangarm64
    do
        if [[ -x "$prefix/bin/gcc.exe" ]]; then
            PREFIX="$prefix"
            CC="$prefix/bin/gcc.exe"
            break
        fi
    done
fi

if [[ -z "$CC" ]]; then
    echo "[-] MinGW GCC not found."
    echo "    Expected one of:"
    echo "      C:/msys64/mingw64/bin/gcc.exe"
    echo "      C:/msys64/ucrt64/bin/gcc.exe"
    exit 1
fi

export PATH="$PREFIX/bin:$PATH"

echo "[+] compiler: $CC"
echo "[+] prefix:   $PREFIX"

# ---- clean -----------------------------------------------------------------

if [[ "$1" == "-Clean" || "$1" == "--clean" ]]; then
    rm -rf "$BUILD" "$ZIP"
    echo "[+] removed build/ and sk.zip"
    exit 0
fi

# ---- checks ----------------------------------------------------------------

command -v zip >/dev/null || {
    echo "[-] zip not found."
    echo "    Install with: pacman -S zip"
    exit 1
}

if [[ ! -f "$PREFIX/include/curl/curl.h" ]]; then
    echo "[-] libcurl not found in:"
    echo "    $PREFIX"
    echo
    echo "Install the appropriate package, e.g.:"
    echo "    pacman -S mingw-w64-x86_64-curl"
    exit 1
fi

target="$("$CC" -dumpmachine)"

if [[ "$target" != *mingw* ]]; then
    echo "[-] compiler target is not MinGW:"
    echo "    $target"
    exit 1
fi

echo "[+] target:   $target"
echo "[+] curl:     $PREFIX"

# ---- compile ---------------------------------------------------------------

rm -rf "$BUILD"
mkdir -p "$BUILD"

sources=(
    sk.c
    net.c
    inst.c
    extract.c
    cjson.c
)

echo "[*] compiling..."

"$CC" \
    -O2 \
    -Wall \
    -Wextra \
    -Wno-format-truncation \
    "-I$PREFIX/include" \
    -o "$BUILD/sk.exe" \
    "${sources[@]}" \
    "-L$PREFIX/lib" \
    -lcurl \
    -lshlwapi \
    -lole32 \
    -lshell32 \
    -luuid

echo "[+] built: $BUILD/sk.exe"

# ---- copy DLL dependencies -------------------------------------------------

echo "[*] copying DLL dependencies..."

declare -A seen
queue=("$BUILD/sk.exe")

while ((${#queue[@]})); do
    file="${queue[0]}"
    queue=("${queue[@]:1}")

    while read -r dll; do
        [[ -z "$dll" ]] && continue

        key="${dll,,}"

        [[ "${seen[$key]}" == 1 ]] && continue
        seen[$key]=1

        # Windows system DLLs
        case "$key" in
            kernel32.dll|kernelbase.dll|ntdll.dll|user32.dll|gdi32.dll|\
            advapi32.dll|shell32.dll|shlwapi.dll|ole32.dll|oleaut32.dll|\
            ws2_32.dll|crypt32.dll|bcrypt.dll|wldap32.dll|normaliz.dll|\
            iphlpapi.dll|secur32.dll|msvcrt.dll|ucrtbase.dll|comctl32.dll|\
            comdlg32.dll|rpcrt4.dll|sechost.dll|imm32.dll|version.dll|\
            api-ms-win-*.dll)
                continue
                ;;
        esac

        if [[ -f "$BUILD/$dll" ]]; then
            queue+=("$BUILD/$dll")
            continue
        fi

        src=""

        if [[ -f "$PREFIX/bin/$dll" ]]; then
            src="$PREFIX/bin/$dll"
        elif [[ -f "$PREFIX/lib/$dll" ]]; then
            src="$PREFIX/lib/$dll"
        fi

        if [[ -z "$src" ]]; then
            echo "[!] missing: $dll"
            continue
        fi

        cp -f "$src" "$BUILD/$dll"
        echo "    + $dll"

        queue+=("$BUILD/$dll")

    done < <(
        objdump -p "$file" 2>/dev/null |
            sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p'
    )
done

# ---- zip -------------------------------------------------------------------

echo "[*] creating sk.zip..."

rm -f "$ZIP"

(
    cd "$ROOT"
    zip -qr "$ZIP" build
)

echo "[+] created: $ZIP"
echo

printf '%s\n' "Contents:"
find "$BUILD" -maxdepth 1 -type f -printf '    %f  %s bytes\n'

