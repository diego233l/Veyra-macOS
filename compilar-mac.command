#!/bin/bash
# Compila Veyra para macOS (Intel, macOS 12 Monterey o posterior; en Apple Silicon
# funciona con Rosetta) y la instala en ~/Applications.
# Doble clic en este archivo o: bash compilar-mac.command
#
# No necesita Homebrew. Descarga en la carpeta .tools/ de este proyecto:
#   - CMake, Ninja, aqtinstall, pkgconf, protobuf y grpcio-tools (entorno virtual de Python)
#   - Qt 6.8.3 para macOS (~1,6 GB) con Qt Multimedia y Qt Shader Tools
#   - vcpkg y las librerías de PS5 Remote Play (OpenSSL, Opus, json-c, miniupnpc,
#     libevent, SDL3, FFmpeg) compiladas para macOS 12; la primera vez tarda ~30-60 min
#   - chiaki-ng en la versión que usa Veyra, con sus parches
# Volver a ejecutarlo reutiliza lo ya descargado y compilado.
#
# Variables opcionales:
#   CI=1            no copia la app a ~/Applications ni la abre (GitHub Actions)
#   VEYRA_ARCH=x86_64|arm64   arquitectura a compilar (por defecto x86_64)
#   VEYRA_REMOTEPLAY=0        compila sin PS5 Remote Play (sin vcpkg ni chiaki-ng)
#   VEYRA_JOBS=N    compilaciones en paralelo
set -euo pipefail

cd "$(dirname "$0")"
ROOT="$(pwd)"
TOOLS="$ROOT/.tools"
QT_VERSION="6.8.3"
QT_DIR="$TOOLS/Qt/$QT_VERSION/macos"
BUILD="$ROOT/build-mac"
APP="$BUILD/Veyra.app"

paso()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()    { printf '\033[32m✔ %s\033[0m\n' "$*"; }
aviso() { printf '\033[33m! %s\033[0m\n' "$*"; }
fallo() { printf '\n\033[1;31m✘ %s\033[0m\n' "$*" >&2; exit 1; }
trap 'printf "\n\033[1;31m✘ Algo ha fallado (línea %s). Revisa el mensaje de arriba.\033[0m\n" "$LINENO" >&2' ERR

[ "$(uname -s)" = "Darwin" ] || fallo "Este script es para macOS."

# --- 0. Sistema ---------------------------------------------------------------
MACOS_VER="$(sw_vers -productVersion)"
MACOS_MAJOR="${MACOS_VER%%.*}"
if [ "$MACOS_MAJOR" -lt 12 ]; then
  fallo "Necesitas macOS 12 Monterey o posterior (tienes $MACOS_VER). Qt 6.8 no funciona en versiones anteriores."
fi
ARCH="${VEYRA_ARCH:-x86_64}"
REMOTEPLAY="${VEYRA_REMOTEPLAY:-1}"
case "$ARCH" in
  x86_64) TRIPLET=x64-osx-12 ;;
  arm64)  TRIPLET=arm64-osx-12 ;;
  *) fallo "VEYRA_ARCH debe ser x86_64 o arm64 (una sola arquitectura: las librerías de Remote Play no son universales)." ;;
esac
JOBS="${VEYRA_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
ok "macOS $MACOS_VER · $ARCH · $JOBS núcleos"

# --- 1. Herramientas de línea de comandos de Xcode -----------------------------
paso "Comprobando las herramientas de línea de comandos de Xcode"
if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find clang >/dev/null 2>&1; then
  aviso "Faltan. Se abrirá el instalador de Apple."
  aviso "Cuando termine (unos 10 minutos), vuelve a hacer doble clic en este script."
  xcode-select --install 2>/dev/null || true
  exit 1
fi
ok "$(xcrun clang --version | head -1)"

# --- 2. CMake, Ninja y aqtinstall en un entorno virtual ------------------------
paso "Preparando CMake, Ninja, pkgconf, protobuf y el descargador de Qt"
mkdir -p "$TOOLS"
# vcpkg's meson (FFmpeg) needs Python >= 3.10; the Xcode tools ship 3.9, so prefer a newer one.
PY="${VEYRA_PYTHON:-}"
if [ -z "$PY" ]; then
  for cand in python3.13 python3.12 python3.11 python3.10 python3 "$(xcrun --find python3 2>/dev/null || true)"; do
    c="$(command -v "$cand" 2>/dev/null || true)"; [ -n "$c" ] || continue
    if "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3,10) else 1)' 2>/dev/null; then PY="$c"; break; fi
  done
fi
if [ -z "$PY" ] && [ "${VEYRA_REMOTEPLAY:-1}" = "0" ]; then PY="$(xcrun --find python3 2>/dev/null || command -v python3 || true)"; fi
[ -n "$PY" ] || fallo "Necesito Python 3.10 o superior para compilar las librerías del Remote Play. Instálalo desde https://www.python.org/downloads/macos/ y vuelve a ejecutar este script."
[ -n "$PY" ] || fallo "No encuentro python3 (debería venir con las herramientas de Xcode)."
VENV="$TOOLS/venv"
if [ ! -x "$VENV/bin/python" ]; then
  "$PY" -m venv "$VENV" || fallo "No se pudo crear el entorno de Python."
fi
if ! "$VENV/bin/python" -c "import aqt, google.protobuf, grpc_tools" >/dev/null 2>&1 || [ ! -x "$VENV/bin/cmake" ] \
   || [ ! -x "$VENV/bin/ninja" ] || [ ! -x "$VENV/bin/pkgconf" ] \
   || "$VENV/bin/cmake" --version 2>/dev/null | head -1 | grep -q 'version [4-9]'; then
  "$VENV/bin/python" -m pip install --quiet --upgrade pip
  # protobuf + grpcio-tools: el generador nanopb de chiaki-ng los usa en lugar de protoc.
  "$VENV/bin/python" -m pip install --quiet "cmake>=3.24,<4" ninja aqtinstall pkgconf protobuf grpcio-tools
fi
export PATH="$VENV/bin:$PATH"
ok "$(cmake --version | head -1) · ninja $(ninja --version)"

# --- 3. Qt 6.8.3 para macOS -----------------------------------------------------
paso "Qt $QT_VERSION para macOS"
if [ ! -f "$QT_DIR/lib/cmake/Qt6/Qt6Config.cmake" ] || [ ! -d "$QT_DIR/lib/QtMultimedia.framework" ] \
   || [ ! -x "$QT_DIR/bin/qsb" ]; then
  aviso "Descargando Qt (~1,6 GB). La primera vez tarda un rato..."
  "$VENV/bin/aqt" install-qt mac desktop "$QT_VERSION" clang_64 \
      -m qtmultimedia qtshadertools -O "$TOOLS/Qt"
  # Los símbolos de depuración (.dSYM) ocupan ~3,5 GB y no hacen falta.
  find "$TOOLS/Qt" -name '*.dSYM' -type d -prune -exec rm -rf {} + 2>/dev/null || true
fi
[ -f "$QT_DIR/lib/cmake/Qt6/Qt6Config.cmake" ] || fallo "Qt no se instaló correctamente en $QT_DIR"
ok "Qt en $QT_DIR"

# --- 4. Librerías de PS5 Remote Play (vcpkg) y chiaki-ng ------------------------
RP_ARGS=(-DVEYRA_ENABLE_REMOTEPLAY=OFF)
if [ "$REMOTEPLAY" = "1" ]; then
  paso "Librerías de PS5 Remote Play ($TRIPLET)"
  # FFmpeg para Intel necesita el ensamblador nasm.
  if [ "$ARCH" = "x86_64" ] && ! command -v nasm >/dev/null 2>&1; then
    NASM_VER=3.01
    NASM_DIR="$TOOLS/nasm-$NASM_VER"
    if [ ! -x "$NASM_DIR/nasm" ]; then
      aviso "Descargando nasm $NASM_VER..."
      curl -fsSL --retry 3 -o "$TOOLS/nasm.zip" \
        "https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VER/macosx/nasm-$NASM_VER-macosx.zip"
      (cd "$TOOLS" && unzip -qo nasm.zip && rm -f nasm.zip)
    fi
    export PATH="$NASM_DIR:$PATH"
  fi
  command -v git >/dev/null 2>&1 || fallo "Falta git (viene con las herramientas de Xcode)."
  BASELINE="$("$VENV/bin/python" -c 'import json;print(json.load(open("vcpkg.json"))["builtin-baseline"])')"
  VCPKG_ROOT="$TOOLS/vcpkg"
  if [ ! -d "$VCPKG_ROOT/.git" ]; then
    git clone --filter=blob:none https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
  fi
  if ! git -C "$VCPKG_ROOT" cat-file -e "$BASELINE^{commit}" 2>/dev/null; then
    git -C "$VCPKG_ROOT" fetch --filter=blob:none origin
  fi
  if [ "$(git -C "$VCPKG_ROOT" rev-parse HEAD)" != "$BASELINE" ]; then
    git -C "$VCPKG_ROOT" checkout -q "$BASELINE"
    rm -f "$VCPKG_ROOT/vcpkg"
  fi
  [ -x "$VCPKG_ROOT/vcpkg" ] || "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
  export VCPKG_ROOT VCPKG_DISABLE_METRICS=1
  export PKG_CONFIG="$VENV/bin/pkgconf"
  VCPKG_INSTALLED="$TOOLS/vcpkg_installed"
  aviso "La primera vez compila OpenSSL, FFmpeg, SDL3... (30-60 min). Después se reutiliza."
  "$VCPKG_ROOT/vcpkg" install \
    --triplet "$TRIPLET" --overlay-triplets="$ROOT/triplets" \
    --x-install-root="$VCPKG_INSTALLED" --clean-after-build
  ok "Librerías listas en $VCPKG_INSTALLED/$TRIPLET"

  paso "chiaki-ng (versión de Veyra 2.1.9 con sus parches)"
  CHIAKI_DIR="$TOOLS/chiaki-ng"
  bash "$ROOT/scripts/fetch-chiaki.sh" "$CHIAKI_DIR"

  export PKG_CONFIG_PATH="$VCPKG_INSTALLED/$TRIPLET/lib/pkgconfig:$VCPKG_INSTALLED/$TRIPLET/share/pkgconfig"
  RP_ARGS=(
    -DVEYRA_ENABLE_REMOTEPLAY=ON
    -DVEYRA_CHIAKI_SOURCE_DIR="$CHIAKI_DIR"
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
    -DVCPKG_TARGET_TRIPLET="$TRIPLET"
    -DVCPKG_OVERLAY_TRIPLETS="$ROOT/triplets"
    -DVCPKG_INSTALLED_DIR="$VCPKG_INSTALLED"
    -DVCPKG_MANIFEST_INSTALL=OFF
    -DPKG_CONFIG_EXECUTABLE="$VENV/bin/pkgconf"
    -DPython3_EXECUTABLE="$VENV/bin/python"
    -DPYTHON_EXECUTABLE="$VENV/bin/python"
  )
fi

# --- 5. Compilar ----------------------------------------------------------------
paso "Configurando el proyecto"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_PREFIX_PATH="$QT_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  "${RP_ARGS[@]}"

paso "Compilando Veyra"
cmake --build "$BUILD" --parallel "$JOBS"
[ -d "$APP" ] || fallo "La compilación no generó $APP"
ok "Compilado: $APP"

# --- 6. Empaquetar Qt dentro de la app ------------------------------------------
paso "Incluyendo Qt dentro de Veyra.app"
"$QT_DIR/bin/macdeployqt" "$APP" -qmldir="$ROOT/qml" -qmldir="$ROOT/mac" -always-overwrite
# macdeployqt solo copia los plugins de multimedia que detecta; nos aseguramos de ambos backends.
mkdir -p "$APP/Contents/PlugIns/multimedia"
for p in libdarwinmediaplugin.dylib libffmpegmediaplugin.dylib; do
  if [ -f "$QT_DIR/plugins/multimedia/$p" ] && [ ! -f "$APP/Contents/PlugIns/multimedia/$p" ]; then
    cp "$QT_DIR/plugins/multimedia/$p" "$APP/Contents/PlugIns/multimedia/"
  fi
done

paso "Firmando la app (firma local, ad hoc)"
xattr -cr "$APP" 2>/dev/null || true
codesign --force --deep --sign - "$APP"
codesign --verify --deep "$APP" && ok "Firma correcta"

# --- 7. Instalar ----------------------------------------------------------------
if [ "${CI:-}" = "1" ]; then
  ok "Modo CI: la app queda en $APP"
  exit 0
fi

paso "Instalando en ~/Applications"
DEST="$HOME/Applications"
mkdir -p "$DEST"
if pgrep -x Veyra >/dev/null 2>&1; then
  aviso "Cerrando la Veyra que estaba abierta..."
  osascript -e 'quit app "Veyra"' 2>/dev/null || pkill -x Veyra || true
  sleep 1
fi
rm -rf "$DEST/Veyra.app"
cp -R "$APP" "$DEST/"
xattr -dr com.apple.quarantine "$DEST/Veyra.app" 2>/dev/null || true

trap - ERR
printf '\n\033[1;32m✔ Veyra instalada en %s/Veyra.app\033[0m\n' "$DEST"
echo "La primera vez que uses la captura de pantalla, la cámara o la capturadora, macOS"
echo "te pedirá permiso (Preferencias del Sistema → Seguridad y privacidad → Privacidad)."
open "$DEST/Veyra.app"
