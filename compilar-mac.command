#!/bin/bash
# Compila Veyra para macOS (Intel o Apple Silicon, macOS 12 Monterey o posterior)
# y la instala en ~/Applications. Doble clic en este archivo o: bash compilar-mac.command
#
# No necesita Homebrew. Descarga en la carpeta .tools/ de este proyecto:
#   - CMake, Ninja y aqtinstall (en un entorno virtual de Python)
#   - Qt 6.8.3 para macOS (~1,6 GB) con Qt Multimedia y Qt Shader Tools
# Volver a ejecutarlo reutiliza lo ya descargado.
#
# Variables opcionales:
#   CI=1            no copia la app a ~/Applications ni la abre (GitHub Actions)
#   VEYRA_ARCH=x86_64|arm64   arquitectura a compilar (por defecto, la del Mac)
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
ARCH="${VEYRA_ARCH:-$(uname -m)}"
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
paso "Preparando CMake, Ninja y el descargador de Qt"
mkdir -p "$TOOLS"
PY="$(xcrun --find python3 2>/dev/null || command -v python3 || true)"
[ -n "$PY" ] || fallo "No encuentro python3 (debería venir con las herramientas de Xcode)."
VENV="$TOOLS/venv"
if [ ! -x "$VENV/bin/python" ]; then
  "$PY" -m venv "$VENV" || fallo "No se pudo crear el entorno de Python."
fi
if ! "$VENV/bin/python" -c "import aqt" >/dev/null 2>&1 || [ ! -x "$VENV/bin/cmake" ] || [ ! -x "$VENV/bin/ninja" ]; then
  "$VENV/bin/python" -m pip install --quiet --upgrade pip
  "$VENV/bin/python" -m pip install --quiet "cmake>=3.24" ninja aqtinstall
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

# --- 4. Compilar ----------------------------------------------------------------
paso "Configurando el proyecto"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_PREFIX_PATH="$QT_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0

paso "Compilando Veyra"
cmake --build "$BUILD" --parallel "$JOBS"
[ -d "$APP" ] || fallo "La compilación no generó $APP"
ok "Compilado: $APP"

# --- 5. Empaquetar Qt dentro de la app ------------------------------------------
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

# --- 6. Instalar ----------------------------------------------------------------
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
