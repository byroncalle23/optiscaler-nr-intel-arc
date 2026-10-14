# Instalación guiada v1.3: sin distribuir el modelo

El asistente reduce el trabajo manual: importa un paquete local, comprueba integridad, obtiene el runtime oficial si falta y registra la configuración. No descarga pesos, no extrae una DLL NVIDIA y no certifica por sí solo el temporal del motor.

## Paquetes de esta versión

| Variante | SHA-256 |
|---|---|
| B580 | `7f823dab904da00f6a392dbbea90b48d214b31aed45d033d7de84a23eb1572b8` |
| B570/B580 experimental | `724f8b317fb5bf71334be04c0b27be432da235d12b8dffee5aaf6efeea335011` |

Son paquetes distintos de v1.2; la release anterior se conserva. B570 continúa sin validación física.

Descarga: [v1.3.0-guided-no-weights](https://github.com/byroncalle23/optiscaler-nr-intel-arc/releases/tag/v1.3.0-guided-no-weights). Los tamaños y SHA-256 de los tres archivos publicados se verificaron contra los originales locales.

## Lo que necesita aportar el usuario

Una carpeta que contenga `native_nr_modes` con los cinco paquetes convertidos compatibles:

```
carpeta-local/
  native_nr_modes/
    assets-180/assets.txt
    assets-288/assets.txt
    assets-360/assets.txt
    assets-540/assets.txt
    assets-720/assets.txt
```

Cada paquete debe contener todos los archivos indicados en su manifiesto. Una DLL NVIDIA suelta, un checkpoint de otro modelo o solo el código de GitHub **no sirven como entrada de este asistente**. El usuario debe disponer de ese paquete con autorización; esta versión no resuelve su adquisición ni su extracción/conversión.

Opcionalmente, la misma carpeta puede contener `native_nr_runtime` con el runtime compatible. Si ya está instalado y verificado en el juego, se conserva.

## Pasos

1. Cerrar el juego, abrir el instalador y seleccionar su ejecutable real.
2. Pulsar **Instalar** para instalar OptiScaler base y registrar los respaldos.
3. Pulsar **Preparar NR** y seleccionar la carpeta del modelo local. Si se selecciona directamente `native_nr_modes`, el asistente reconoce su carpeta superior.
4. Si falta el runtime, autorizar la descarga de **Microsoft.Direct3D.D3D12 1.721.3-preview** desde `api.nuget.org`. Leer la licencia mostrada y aceptarla antes de copiar el runtime al juego. Cancelar deja NR sin preparar. El instalador no incluye ni redistribuye ese SDK.
5. Esperar la validación/importación y el registro. Se verifican geometrías, rutas, tamaños, SHA-256 y los hashes del runtime esperado. Se comparten archivos iguales mediante enlaces NTFS cuando es posible.
6. Ejecutar `Launch-OptiScaler-NR.bat` como administrador, dejando su ventana abierta. Dentro del juego, Insert → Custom → activar NR; empezar por 320×180.
7. Comprobar imagen y estado temporal con NR apagado/encendido. Preparar archivos no demuestra que un juego nuevo tenga un puente temporal correcto.

Durante la importación NR permanece desactivado. Un error no habilita el modelo parcialmente importado. Se puede repetir la preparación con el juego cerrado; los archivos copiados quedan registrados y respaldados para su restauración. No cerrar el instalador durante una operación.

## Perfiles y límites

| Ejecutable reconocido | Tratamiento |
|---|---|
| `Spider-Man2.exe` | Referencia general; configura el ajuste Dxgi del perfil existente y pide comprobación en juego |
| `re9.exe` | Detectado como Requiem y bloqueado en este paquete; mantiene la integración específica con REFramework |
| `re2.exe`, `re3.exe`, `re4.exe` | Aviso experimental: requiere ruta DX12 y el temporal no está validado para esos títulos |
| Otro | Ruta DX12 SDR experimental; motor no confirmado |

La identificación por nombre es una ayuda de configuración, no una identificación infalible del motor ni una validación de versión. **No se instala REFramework automáticamente**, ni se trasplantan parches de Requiem a otros Resident Evil. Tampoco se instalan drivers GPU ni se garantiza compatibilidad de una versión diferente del juego.

## Restauración

Cerrar el juego y usar Restaurar/desinstalar. Los archivos que el asistente importó se retiran o restauran conforme al registro; los que ya existían idénticos se conservan. Se preservan y notifican modificaciones posteriores del usuario. La carpeta fuente elegida por el usuario no se modifica.

## Evidencia de pruebas

Las pruebas son aisladas, con el juego cerrado: no equivalen a una sesión real en otro equipo. La comprobación del SDK descarga el paquete oficial y verifica que sus tres binarios esperados coinciden con el contrato; no activa opciones de Windows ni ejecuta la inferencia. La aceptación de términos y el selector de carpetas requieren interacción del usuario y se revisan como parte de la interfaz.

Resultado: **34 comprobaciones pasadas** entre ambas variantes, incluidos perfiles, modelo ausente, geometría inválida, descarga/extracción del SDK oficial, importación automática real de cinco paquetes, registro y desinstalación restaurando archivos anteriores. No se cambiaron instalaciones reales de juegos ni ajustes de Windows. La interfaz principal se inspeccionó mediante preview; no se automatizó toda la interacción de aceptación de licencia en una sesión de usuario.

Código: `installer/GuidedSetup.cs`, `installer/GenericInstaller.cs`, `tools/build_public_installers.py --guided` y `tools/test_public_installers.py --guided`.
