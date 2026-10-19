#!/usr/bin/env bash

set -euo pipefail

project_source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd)"
project_source_dir="$(cygpath -u "${project_source_dir}")"
build_dir="${XEMU_UWP_BUILD_DIR:-${project_source_dir}/build-uwp-msys2}"
dist_dir="${XEMU_UWP_DIST_DIR:-${project_source_dir}/dist-uwp-msys2}"
sdl3_uwp_dir="${XEMU_SDL3_UWP_DIR:-G:/Downloads/SDL3-UWP/%INSTALLDIR%}"
target="qemu-system-i386w.exe"
jobs="${JOBS:-${NUMBER_OF_PROCESSORS:-4}}"

case "$(uname -s)" in
    MINGW*_NT-*|MSYS_NT-*|CYGWIN_NT-*) ;;
    *)
        echo "This script must run from an MSYS2/MinGW shell on Windows." >&2
        exit 1
        ;;
esac

if test "${MSYSTEM:-}" != "UCRT64"; then
    echo "This UWP build must run from the MSYS2 UCRT64 environment." >&2
    echo "Using MINGW64 can produce an xemu.exe that imports msvcrt.dll, which Xbox UWP refuses to activate." >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to prepare ScreenScraper build credentials." >&2
    exit 1
fi

mkdir -p "${build_dir}"
python3 "${project_source_dir}/scripts/generate-screenscraper-build-secrets.py" \
    "${build_dir}/screenscraper-build-secrets.h"

if ! test -f /ucrt64/lib/libwindowsapp.a && ! test -f /mingw64/lib/libwindowsapp.a; then
    echo "libwindowsapp.a not found. Install the MSYS2 MinGW-w64 CRT package." >&2
    exit 1
fi

if ! test -f /ucrt64/lib/libwinstorecompat.a && ! test -f /mingw64/lib/libwinstorecompat.a; then
    echo "libwinstorecompat.a not found. Install the MSYS2 winstorecompat package." >&2
    exit 1
fi

uwp_cflags="-DXBOX=1 -DXEMU_UWP=1 -DSDL_PLATFORM_WINRT=1 -DSDL_WINRT_METADATA_FILE_AVAILABLE=1 -DWINAPI_FAMILY=WINAPI_FAMILY_APP -DWINSTORECOMPAT=1 -D__WINRT__=1 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A00000B -Wno-error"
uwp_cxxflags="${uwp_cflags}"
uwp_ldflags="-lwinstorecompat -lwindowsapp"

find_llvm_ar() {
    local candidate

    if test -n "${XEMU_AR:-}"; then
        printf "%s\n" "${XEMU_AR}"
        return 0
    fi

    for candidate in \
        "C:/Progra~1/LLVM/bin/llvm-ar.exe" \
        "/c/Progra~1/LLVM/bin/llvm-ar.exe" \
        "/c/Program Files/LLVM/bin/llvm-ar.exe" \
        "/c/Program Files (x86)/LLVM/bin/llvm-ar.exe" \
        "$(command -v llvm-ar 2>/dev/null || true)"; do
        if test -n "${candidate}" && test -x "${candidate}"; then
            printf "%s\n" "${candidate}"
            return 0
        fi
    done

    return 1
}

find_cppwinrt_include() {
    local root
    local found

    if test -n "${XEMU_CPPWINRT_INCLUDE:-}"; then
        if test -f "${XEMU_CPPWINRT_INCLUDE}/winrt/base.h"; then
            printf "%s\n" "${XEMU_CPPWINRT_INCLUDE}"
            return 0
        fi
        echo "XEMU_CPPWINRT_INCLUDE does not contain winrt/base.h: ${XEMU_CPPWINRT_INCLUDE}" >&2
        return 1
    fi

    for root in \
        "/c/Program Files (x86)/Windows Kits/10/Include" \
        "/c/Program Files/Windows Kits/10/Include"; do
        if ! test -d "${root}"; then
            continue
        fi
        while IFS= read -r candidate; do
            if test -f "${candidate}/winrt/base.h"; then
                found="${candidate}"
            fi
        done < <(find "${root}" -mindepth 2 -maxdepth 2 -type d -name cppwinrt 2>/dev/null | sort -V)
    done

    if test -n "${found:-}"; then
        printf "%s\n" "${found}"
        return 0
    fi

    return 1
}

if ar_path="$(find_llvm_ar)"; then
    export AR="${ar_path}"
    echo "Using archiver ${AR}"
fi

sdl3_uwp_dir="$(cygpath -u "${sdl3_uwp_dir}")"
sdl3_uwp_source_dll="${sdl3_uwp_dir}/bin/SDL3.dll"

if ! test -f "${sdl3_uwp_source_dll}"; then
    echo "SDL3-UWP DLL not found at ${sdl3_uwp_source_dll}." >&2
    echo "Set XEMU_SDL3_UWP_DIR to the installed SDL3-UWP directory." >&2
    exit 1
fi

if ! test -f "${sdl3_uwp_dir}/lib/pkgconfig/sdl3.pc"; then
    echo "SDL3-UWP pkg-config file not found at ${sdl3_uwp_dir}/lib/pkgconfig/sdl3.pc." >&2
    exit 1
fi

if cppwinrt_include="$(find_cppwinrt_include)"; then
    cppwinrt_stage_dir="${build_dir}/cppwinrt"
    rm -rf "${cppwinrt_stage_dir}"
    mkdir -p "${cppwinrt_stage_dir}"
    cp -R "${cppwinrt_include}/." "${cppwinrt_stage_dir}/"
    uwp_cxxflags="${uwp_cxxflags} -std=c++20 -isystem $(cygpath -m "${cppwinrt_stage_dir}")"
    echo "Using C++/WinRT headers from ${cppwinrt_include}"
else
    echo "C++/WinRT headers not found. Install the Windows SDK or set XEMU_CPPWINRT_INCLUDE." >&2
    exit 1
fi
sdl3_uwp_stage_dir="${build_dir}/sdl3-uwp-installed"
rm -rf "${sdl3_uwp_stage_dir}"
mkdir -p "${sdl3_uwp_stage_dir}"
cp -R "${sdl3_uwp_dir}/." "${sdl3_uwp_stage_dir}/"

sdl3_uwp_dll="${sdl3_uwp_stage_dir}/bin/SDL3.dll"
sdl3_uwp_libdir="${sdl3_uwp_stage_dir}/lib"
sdl3_uwp_pcdir="${sdl3_uwp_libdir}/pkgconfig"
sdl3_uwp_import_lib="${sdl3_uwp_libdir}/libSDL3.dll.a"

if ! test -f "${sdl3_uwp_import_lib}"; then
    command -v gendef >/dev/null 2>&1 || {
        echo "gendef not found. Install mingw-w64-ucrt-x86_64-tools-git or ensure /ucrt64/bin is in PATH." >&2
        exit 1
    }
    command -v dlltool >/dev/null 2>&1 || {
        echo "dlltool not found. Ensure the MSYS2 MinGW toolchain is in PATH." >&2
        exit 1
    }
    (
        cd "${sdl3_uwp_libdir}"
        gendef "${sdl3_uwp_dll}"
        dlltool -d SDL3.def -D SDL3.dll -l libSDL3.dll.a
    )
fi

export PKG_CONFIG_PATH="${sdl3_uwp_pcdir}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
echo "Using SDL3-UWP DLL from ${sdl3_uwp_source_dll}"

cd "${build_dir}"
prefix_dir="$(cygpath -m "${dist_dir}/prefix")"

"${project_source_dir}/configure" \
    --prefix="${prefix_dir}" \
    --extra-cflags="${uwp_cflags} ${CFLAGS:-}" \
    --extra-cxxflags="${uwp_cxxflags} ${CXXFLAGS:-}" \
    --extra-ldflags="${uwp_ldflags} ${LDFLAGS:-}" \
    --target-list=i386-softmmu \
    --enable-uwp \
    --enable-d3d12 \
    --disable-ffmpeg \
    --enable-sdl \
    --disable-opengl \
    --disable-vulkan \
    --disable-dsound \
    --audio-drv-list=sdl \
    -Dbuildtype=custom \
    -Doptimization=3 \
    -Db_ndebug=false \
    --disable-debug-info \
    --disable-qom-cast-debug \
    --disable-werror \
    "$@"

if test -n "${AR:-}"; then
    find "${build_dir}" -type f -name '*.a' \
        ! -path "${sdl3_uwp_stage_dir}/*" \
        ! -path "${build_dir}/subprojects/dsp56300/*" -delete
fi

time make -j"${jobs}" "${target}" 2>&1 | tee "${build_dir}/build.log"

mkdir -p "${dist_dir}"
rm -f "${dist_dir}/xemu.exe" "${dist_dir}/SDL3.dll"
rm -rf "${dist_dir}/prefix"
cp "${build_dir}/${target}" "${dist_dir}/xemu.exe"
cp "${sdl3_uwp_dll}" "${dist_dir}/SDL3.dll"

echo "Built ${dist_dir}/xemu.exe"
echo "Package with: powershell -ExecutionPolicy Bypass -File scripts/package-uwp-msys2.ps1 -BuildDir '${build_dir}' -DistDir '${dist_dir}'"
