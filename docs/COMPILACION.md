# Compilación y reproducción

Esta publicación es una instantánea de investigación. No incluye `work/`, pesos, paquetes de modelo, dependencias binarias ni los clones de terceros. Los scripts históricos del laboratorio pueden requerir adaptación de rutas; no se promete un build limpio de un solo comando.

## Dependencias y origen

- OptiScaler: https://github.com/optiscaler/OptiScaler , commit `7651154215172af82d9d39ef5efb3798ff1e3703`.
- Trabajo de referencia Intel/Linux: https://github.com/Uzbekunknown/dlss-nr-on-intel . Conservar atribución y licencia Apache-2.0.
- Referencia independiente de recuperación: https://github.com/iamwavecut/MLX-DLSS . Sus pesos no forman parte del código Apache.
- Microsoft Direct3D experimental SDK 721.3: paquete `Microsoft.Direct3D.D3D12` versión `1.721.3-preview`. Revisar condiciones del paquete antes de distribuir sus binarios.
- Windows SDK 10.0.26100.0, herramientas MSVC/v145 utilizadas en la compilación local, Python 3.11+ para scripts, .NET Framework 4.x para el instalador.

## Preparar OptiScaler

Clonar OptiScaler en `work/optiscaler-source`, fijar el commit y obtener sus dependencias/submódulos conforme a las instrucciones del proyecto. Aplicar `patches/optiscaler-integration.patch` sobre ese commit. Copiar las cabeceras de `integration/optiscaler` en `OptiScaler/experimental` como hace el script; las cabeceras nuevas no incluidas en el diff se proporcionan aparte.

Obtener localmente los headers de SDK721 en `work/b580/cooperative-vectors/agility7213/build/native/include`. No sustituir el D3D12Core original del juego por la versión experimental.

Desde la raíz:

```powershell
python tools/embed_native_shaders.py --check
./tools/build_optiscaler_isolated.ps1
./tools/build_optiscaler_isolated.ps1 -BSeries
```

La primera compilación genera la referencia de ordinales usada por el build con modelo. La segunda fija los defines aceptados y genera `global-b1-modes-bseries/bin/OptiScaler.dll`. La variante `-Modes` genera la carpeta histórica de modos, pero el código actual de backend también admite B570. Los binarios históricos E36 no se reproducen exactamente simplemente pasando ese argumento.

**No hay promesa de SHA idéntico:** build timestamps, toolchain y dependencias pueden cambiar el binario. Comparar exportaciones y comportamiento, y registrar el SHA nuevo en el manifiesto de publicación.

## Datos locales necesarios

El grafo consume `graph.txt`, metadatos de forma y `assets.txt` más archivos del modelo. `NativeNrAssets` valida tamaños/rutas/hashes. Las cinco geometrías comparten pesos inmutables, pero requieren formas, ruido y shaders compatibles con su campo. No redimensionar arrays de forma indiscriminada ni confundir resolución lógica con campo con padding.

El script histórico `tools/package_native_model.py` crea un paquete desde una fixture local y excluye capturas de entrada/salida. No obtiene derechos sobre los datos que procesa. La publicación no incluye un extractor ni un método para eludir protecciones del juego.

## Instalador

`installer/GenericInstaller.cs` implementa el motor y la GUI. Un instalador completo añade un ZIP con `package.json` y entradas `blobs/<sha256>`. Al final se añaden 16 bytes de marcador `NRSETUPGENV1PACK` y dos enteros signed little-endian de 64 bits: offset y longitud del ZIP.

Cada item especifica `path`, `sha256`, `blob`, `bytes`, `mode` y `component`. Los pesos compartidos se almacenan una vez en el ZIP y pueden enlazarse en NTFS al instalar. El uninstall usa el mismo motor sin el payload.

`tools/build_generic_installer.py` es el empaquetador local histórico: obtiene bibliotecas y modelos de una instalación local y fija el hash de la DLL aceptada. Para otro equipo, convertir esas rutas en argumentos/configuración y fijar hashes nuevos después de validar. El archivo no es un descargador de modelos y no se debe ejecutar suponiendo que el clon público contiene sus entradas.

No modificar la referencia ni empaquetar pesos para una release pública sin autorización. Los instaladores sin pesos, cuando se publiquen, deben indicar expresamente que NR no estará listo hasta aportar el modelo local; no basta con borrar los `.bin` y anunciar que funcionan.

## Verificación mínima

1. Compilar sin activar variantes descartadas.
2. Comparar nombres y ordinales de exports con la referencia.
3. Ejecutar consultas reales de SM6.10 y formas matriciales.
4. Banco GPU por geometría: finitud, alfa, historias, resets y ausencia de readback en el intervalo medido.
5. Instalador: integridad, rutas, respaldo, rollback, archivos modificados, proceso abierto y desinstalación.
6. Prueba del callback real en juego: A/B, cambio de modo, resize, alt-tab, reinicio y cierre.

No publicar un benchmark de inferencia como FPS del juego ni declarar B570 validada a partir de un test de su identificador PCI.
