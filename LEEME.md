# Veyra para macOS

Port a macOS de **Veyra 2.1.9**, el reproductor y procesador de vídeo de
[Likely7/Veyra-NRVideo](https://github.com/Likely7/Veyra-NRVideo), con licencia **GPL-3.0**
(ver `LICENSE`).

Usa la **misma interfaz** que la versión de Windows (los archivos QML originales, sin cambios)
sobre un motor nuevo escrito para macOS con Qt 6.8: Qt Multimedia para reproducir y capturar,
y shaders propios (Metal a través de Qt) para las mejoras de imagen.

Funciona en Macs **Intel y Apple Silicon** con **macOS 12 Monterey o posterior**
(probado como objetivo: MacBook Pro 2015 con macOS 12).

## Instalación (recomendada)

1. Ve a **Releases** de este repositorio y descarga **`Veyra-macOS.dmg`**
   (la versión `latest-mac` se actualiza sola con cada cambio).
2. Abre el `.dmg` y **arrastra Veyra a la carpeta Aplicaciones**.
3. **La primera vez**, abre Veyra con **clic derecho → Abrir → Abrir**.
   La app no está firmada con un certificado de Apple, así que con doble clic macOS diría
   que «no se puede abrir». Solo hace falta una vez.
4. Concede los permisos cuando macOS los pida, o actívalos a mano en
   **Preferencias del Sistema → Seguridad y privacidad → Privacidad**:
   - **Grabación de pantalla**: para «Captura de pantalla» (ventana o pantalla completa).
     Después de activarlo hay que cerrar y volver a abrir Veyra.
   - **Cámara**: para capturadoras HDMI/USB y webcams.
   - **Micrófono**: para el audio de la capturadora y el micrófono en grabaciones.

Si macOS sigue bloqueándola: Preferencias del Sistema → Seguridad y privacidad → General →
«Abrir igualmente». O en Terminal: `xattr -dr com.apple.quarantine /Applications/Veyra.app`.

### Cómo se genera el .dmg

GitHub Actions (`.github/workflows/macos.yml`) compila una app Intel x86_64 (mínimo macOS 12;
en Apple Silicon funciona con Rosetta) con PS5 Remote Play incluido, en cada
push a `main`/`master` y al crear una etiqueta `v*`. También se puede lanzar a mano
(Actions → *macOS build* → *Run workflow*). El resultado queda como artefacto de la ejecución
y en la release `latest-mac` (o en la release de la etiqueta).

## Compilar en tu propio Mac (alternativa)

Doble clic en **`compilar-mac.command`** (o `bash compilar-mac.command` en Terminal).

- Si faltan las herramientas de línea de comandos de Xcode, abre su instalador: cuando acabe,
  vuelve a ejecutar el script.
- No necesita Homebrew. Descarga en `.tools/` CMake, Ninja, Qt 6.8.3 (~1,6 GB), vcpkg con las
  librerías de PS5 Remote Play (OpenSSL, Opus, json-c, miniupnpc, libevent, SDL3, FFmpeg) y
  chiaki-ng en la versión de Veyra 2.1.9 con sus parches.
  La primera vez tarda **40–80 minutos** (compila FFmpeg y OpenSSL); las siguientes reutiliza todo.
- `VEYRA_REMOTEPLAY=0 bash compilar-mac.command` compila sin Remote Play (unos 15 minutos).
- Al terminar, instala la app en `~/Applications/Veyra.app` y la abre.

## Qué funciona en macOS

| Función | macOS | Notas |
| --- | --- | --- |
| Interfaz completa de Veyra (inicio, minimalista, profesional, nodos, ajustes, idiomas) | ✅ | Mismos QML que en Windows |
| Música e intro de inicio | ✅ | |
| Reproducir vídeo (MP4, MKV, MOV…), imágenes y direcciones de red | ✅ | Qt Multimedia (FFmpeg / AVFoundation) |
| LUT 3D (.cube), exposición, contraste, saturación, temperatura y resto del color | ✅ | Shaders propios |
| Antialiasing (FXAA), nitidez y escalado espacial | ✅ | |
| Comparar con el original, girar imagen, capturas de pantalla | ✅ | |
| Captura de pantalla / ventana | ✅ | Requiere permiso de Grabación de pantalla |
| Capturadoras HDMI y webcams | ✅ | Las reconocidas por macOS (UVC/AVFoundation) |
| Grabar la captura | ✅ | H.264 / H.265 |
| DLSS, RTX Video Super Resolution / HDR | ❌ | Solo NVIDIA en Windows |
| Reducción de ruido neuronal (NR) | ❌ | SDK de NVIDIA / AMD para Windows |
| FSR 3.1 / FSR 4, AMD FRC | ❌ | Drivers AMD de Windows |
| XeSS | ❌ | Solo Windows |
| Generación de fotogramas (FG) | ❌ | Depende de los SDK anteriores |
| Remote Play PS5, Xbox, Moonlight | ❌ | No portado |
| Cámara virtual, búfer de repetición, audio virtual | ❌ | Controladores de Windows |
| Exportar vídeo con las mejoras aplicadas | ❌ | No portado |

Las funciones no disponibles aparecen desactivadas en la interfaz con su motivo.

## Problemas

- **Registro (log):** `~/Library/Application Support/Veyra/Veyra/logs/veyra.log`
  (en Finder: Ir → Ir a la carpeta…, y pega la ruta). Ajustes → «Abrir carpeta de registros»
  lleva al mismo sitio.
- **La captura de pantalla sale negra:** falta el permiso de Grabación de pantalla, o no se
  reinició Veyra después de darlo.
- **La capturadora no aparece:** comprueba que macOS la ve (Información del sistema → USB/Cámara)
  y que Veyra tiene permiso de Cámara.
- **«Veyra está dañada y no se puede abrir»:** es la cuarentena de macOS en apps sin firmar.
  Ejecuta `xattr -dr com.apple.quarantine /Applications/Veyra.app`.
- **Error al compilar con el script:** borra la carpeta `build-mac/` y vuelve a ejecutarlo.
  Si el fallo está en la descarga de Qt, borra también `.tools/Qt/`.

## Licencia

GPL-3.0, igual que Veyra. La música de inicio (`media/startup-music.mp3`) acompaña a los paquetes
ejecutables de Veyra y no forma parte del código bajo GPL.
