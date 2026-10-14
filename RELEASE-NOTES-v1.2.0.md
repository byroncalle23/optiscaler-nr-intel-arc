# OptiScaler NR: instaladores públicos sin pesos

Primera prerelease pública para Intel Arc:

- **B580:** variante de referencia.
- **B570/B580:** admite B570 de forma experimental, sin validación física.

Incluye DLL de integración, bibliotecas redistribuibles de OptiScaler, licencias, lanzador y respaldo/restauración. Ambos instaladores pasaron 20 comprobaciones aisladas en total. No se lanzó un juego ni se volvió a medir rendimiento GPU durante esas pruebas.

**Esta descarga no incluye pesos ni el SDK experimental Microsoft 721. No permite activar NR por sí sola.** OptiScaler base puede usarse con NR apagado. Para NR hay que aportar localmente un modelo compatible y el runtime obtenido de su proveedor, y pulsar «Modelo local» en el instalador para verificarlos y registrar el perfil. NR permanece desactivado inicialmente.

Los cinco modos disponibles al aportar el modelo son 320×180, 512×288, 640×360, 960×540 y 1280×720. Los modos altos pueden exceder memoria disponible o resultar demasiado lentos.

**No hay compatibilidad universal ni garantía de FPS.** Spider-Man 2 es la referencia de la ruta general; Requiem requiere su integración específica y el instalador general lo rechaza. Se conservan los instaladores completos locales y las instalaciones del autor.

Leer antes de instalar: [contenido, requisitos y aporte local](https://github.com/byroncalle23/optiscaler-nr-intel-arc/blob/main/docs/INSTALADORES-PUBLICOS.md).

Fuente correspondiente y documentación: [repositorio](https://github.com/byroncalle23/optiscaler-nr-intel-arc/tree/e990725046e5e0b7e22374af9e150e3eebc2f05b).

Comprobar las descargas con `PUBLIC-INSTALLERS-SHA256.txt`. Los dos ejecutables son variantes alternativas: instala solo una.
