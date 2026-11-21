#!/bin/bash
# Crea un .dmg con Veyra.app y un acceso directo a /Applications para arrastrar.
# Uso: scripts/make-dmg.sh <Veyra.app> <salida.dmg>
set -euo pipefail
APP="$1"; OUT="$2"
[ -d "$APP" ] || { echo "No existe $APP" >&2; exit 1; }
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$OUT"
hdiutil create -volname "Veyra" -srcfolder "$STAGE" -ov -format UDZO -fs HFS+ "$OUT"
echo "DMG: $OUT"
