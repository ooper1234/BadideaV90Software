#!/usr/bin/env bash
set -euo pipefail

if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
  echo "[ERROR] This script must run in an MSYS2 UCRT64 environment." >&2
  exit 2
fi

PACMAN_LOCK=/var/lib/pacman/db.lck

pacman_is_running() {
  # tasklist.exe is available on every supported Windows host and catches a
  # pacman process even when it belongs to another MSYS2 terminal/session.
  tasklist.exe /FI "IMAGENAME eq pacman.exe" 2>/dev/null | tr -d '\r' | grep -qi 'pacman.exe'
}

prepare_pacman_lock() {
  if [[ ! -e "$PACMAN_LOCK" ]]; then
    return 0
  fi

  if pacman_is_running; then
    echo "[MSYS2] Another pacman process owns the package database; waiting for it to finish..."
    for _ in {1..30}; do
      sleep 2
      if ! pacman_is_running; then
        break
      fi
    done
  fi

  if [[ -e "$PACMAN_LOCK" ]]; then
    if pacman_is_running; then
      echo "[ERROR] pacman is still running after 60 seconds. Close the other MSYS2/package-manager window and retry." >&2
      return 1
    fi
    echo "[MSYS2] Removing stale pacman database lock: $PACMAN_LOCK"
    rm -f "$PACMAN_LOCK"
  fi
}

install_msys_packages() {
  local rc=0
  for attempt in 1 2 3; do
    prepare_pacman_lock || return 1
    set +e
    pacman -Sy --needed --noconfirm \
      git make \
      mingw-w64-ucrt-x86_64-gcc \
      mingw-w64-ucrt-x86_64-cmake \
      mingw-w64-ucrt-x86_64-ninja \
      mingw-w64-ucrt-x86_64-pkgconf \
      mingw-w64-ucrt-x86_64-spandsp \
      mingw-w64-ucrt-x86_64-openssl \
      mingw-w64-ucrt-x86_64-jsoncpp
    rc=$?
    set -e
    if (( rc == 0 )); then
      return 0
    fi
    if [[ -e "$PACMAN_LOCK" ]]; then
      echo "[MSYS2] pacman attempt $attempt hit the database lock; retrying..."
      sleep 2
      continue
    fi
    return "$rc"
  done
  return "$rc"
}

install_msys_packages

export PATH="/ucrt64/bin:$PATH"
ROOT="$PWD"
PJROOT="$ROOT/third_party/pjproject"
PJPREFIX="$ROOT/.pjprefix"
PJPATCH="$ROOT/third_party/patches/pjproject-pcmu-negative-zero.patch"
PJMARKER="$PJPREFIX/.v92isp-pcmu-negative-zero"

if [[ ! -f "$PJPREFIX/lib/pkgconfig/libpjproject.pc" || ! -f "$PJMARKER" ]]; then
  echo "[PJSIP] Building patched PJSIP 2.17 (cached in .pjprefix)..."
  mkdir -p "$ROOT/third_party"

  # Keep a successfully downloaded tree between retries. Re-cloning a 10+ MB
  # dependency because MSYS2's optional dependency scanner failed is wasteful.
  if [[ ! -d "$PJROOT/.git" ]]; then
    rm -rf "$PJROOT"
    git clone --depth 1 --branch 2.17 https://github.com/pjsip/pjproject.git "$PJROOT"
  fi

  cd "$PJROOT"

  # V.90 Sd/S-bar uses both PCMU zero codewords. PJSIP's generated fast
  # linear-to-u-law table normally maps every near-zero negative sample to
  # 0x7e, so a decoded 0x7f cannot survive the AudioMediaPort PCM round trip.
  # Apply the small, version-controlled table fix before every fresh install.
  if git apply --check "$PJPATCH" >/dev/null 2>&1; then
    git apply "$PJPATCH"
  elif ! git apply --reverse --check "$PJPATCH" >/dev/null 2>&1; then
    echo "[ERROR] The PJSIP PCMU negative-zero patch does not match this source tree." >&2
    exit 3
  fi

  rm -rf "$PJPREFIX"

  # PJSUA2/AudioMediaPort is built by the normal PJSIP build. We do not need
  # video, which keeps the Windows build smaller and avoids unrelated deps.
  if [[ ! -f build.mak ]]; then
    ./configure --prefix="$PJPREFIX" --disable-video
  fi

  # `make dep` only generates dependency files. On some current MSYS2/MinGW
  # installations GCC can fail while writing the enormous generated .depend
  # stream ("when writing output to : Invalid argument"). This must not stop
  # the actual first build. The normal make rules can compile from source
  # without pre-generated dependency files; they are mainly for incremental
  # rebuild tracking.
  echo "[PJSIP] Generating dependency files (optional on this Windows build)..."
  set +e
  make dep
  dep_rc=$?
  set -e
  if (( dep_rc != 0 )); then
    echo "[PJSIP] WARNING: make dep failed with exit $dep_rc; continuing with direct build."
    echo "[PJSIP] This is a known-safe installer fallback: dependency files are not required for the initial compile."
    # Remove half-written dependency files so GNU make cannot parse a truncated
    # file from the failed scanner pass.
    find . -type f -name '.*.depend' -delete 2>/dev/null || true
  fi

  echo "[PJSIP] Compiling libraries/applications..."
  make -j "$(nproc)"
  echo "[PJSIP] Installing into .pjprefix..."
  make install
  touch "$PJMARKER"
  cd "$ROOT"
fi

# PJSIP's MSYS2 install writes POSIX-style /c/... paths into libpjproject.pc.
# CMake/Ninja invoke the native MinGW compiler directly, where /c/... is not a
# valid Windows include/library path. Normalize the .pc file to C:/... and also
# pass the native prefix explicitly to CMake as a belt-and-suspenders fallback.
PC_FILE="$PJPREFIX/lib/pkgconfig/libpjproject.pc"
if [[ ! -f "$PJPREFIX/include/pjsua2.hpp" ]]; then
  echo "[PJSIP] pjsua2.hpp missing from prefix; repairing installed headers from source tree..."
  mkdir -p "$PJPREFIX/include"
  cp -RLf "$PJROOT/pjsip/include/"* "$PJPREFIX/include/"
fi
if [[ ! -f "$PJPREFIX/include/pjsua2.hpp" ]]; then
  echo "[ERROR] PJSIP built, but pjsua2.hpp is still missing from $PJPREFIX/include" >&2
  exit 3
fi

PJPREFIX_WIN="$(cygpath -m "$PJPREFIX")"
if [[ -f "$PC_FILE" ]]; then
  # Replace every exact MSYS prefix occurrence; library and include flags then
  # become native C:/... paths understood by MinGW when launched by Ninja.
  sed -i "s|$PJPREFIX|$PJPREFIX_WIN|g" "$PC_FILE"
fi

export PKG_CONFIG_PATH="$PJPREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export V92_PJSIP_PREFIX="$PJPREFIX_WIN"
if ! pkg-config --exists libpjproject; then
  echo "[ERROR] PJSIP built but libpjproject.pc is unavailable." >&2
  exit 3
fi

echo "[PJSIP] $(pkg-config --modversion libpjproject 2>/dev/null || echo installed)"
echo "[PJSIP] native include: $PJPREFIX_WIN/include"
echo "[PJSIP] pkg-config cflags: $(pkg-config --cflags libpjproject)"

rm -rf build-windows
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows -j "$(nproc)"

mkdir -p dist
if [[ ! -f build-windows/v92isp-windows-pjsip.exe ]]; then
  echo "[ERROR] PJSIP Windows executable was not produced." >&2
  exit 4
fi
cp -f build-windows/v92isp-windows-pjsip.exe dist/v92isp-windows.exe
cp -f build-windows/v92isp-windows-pjsip.exe dist/v92isp-windows-pjsip.exe

# Copy MSYS2 runtime DLLs needed by SpanDSP/OpenSSL. PJSIP itself is linked
# from the local static libpjproject install.
while IFS= read -r dll; do
  [[ -f "$dll" ]] || continue
  cp -f "$dll" dist/
done < <(ldd build-windows/v92isp-windows-pjsip.exe | awk '{print $3}' | grep -E '^/ucrt64/bin/.*\.dll$' || true)

printf '[OK] PJSIP Windows executable built: %s\\dist\\v92isp-windows.exe\n' "$(pwd -W 2>/dev/null || pwd)"
