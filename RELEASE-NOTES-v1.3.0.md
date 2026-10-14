# Instalación guiada de NR, sin pesos incluidos

Dos variantes: B580 y B570/B580 experimental. Se conserva la release v1.2 anterior.

## Flujo para el usuario

1. Con el juego cerrado, seleccionar su ejecutable y pulsar **Instalar**.
2. Pulsar **Preparar NR** y seleccionar una carpeta de modelo local convertido compatible con los cinco modos. Una DLL NVIDIA suelta no sirve; no se descargan ni extraen pesos.
3. Si falta el runtime, el asistente ofrece descargar Microsoft.Direct3D.D3D12 1.721.3-preview desde NuGet oficial, muestra sus términos y requiere aceptación antes de instalarlo.
4. Importar/verificar archivos y registrar el perfil automáticamente. NR sigue apagado inicialmente.
5. Iniciar el lanzador y activar NR desde el menú, verificando su temporal dentro del juego.

Durante la importación NR se mantiene desactivado; los archivos copiados quedan registrados para restauración. El paquete público no contiene pesos ni binarios del SDK experimental.

Spider-Man 2 es la referencia general. RE2/3/4 son experimentales/DX12, sin temporal validado. Requiem sigue bloqueado en este instalador general y necesita su integración específica. No se instala REFramework automáticamente ni se promete compatibilidad universal.

[Guía completa y requisitos del modelo local](https://github.com/byroncalle23/optiscaler-nr-intel-arc/blob/main/docs/INSTALACION-GUIADA.md).

Se incluyen los SHA-256 para verificar ambas descargas. Instalar solo una variante. B570 no se ha probado físicamente y no hay mediciones nuevas de FPS.
