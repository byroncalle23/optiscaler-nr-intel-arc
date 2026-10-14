# Instaladores públicos sin pesos

Los dos paquetes están publicados como [prerelease v1.2.0](https://github.com/byroncalle23/optiscaler-nr-intel-arc/releases/tag/v1.2.0-public-no-weights). Se verificaron los SHA-256 y tamaños de los tres archivos subidos, incluidos los checksums. **No activan NR por sí solos:** no incluyen pesos ni el runtime experimental Microsoft 721. Sus condiciones de preview no permiten redistribuir ese runtime; debe obtenerse de su proveedor para uso local. El paquete oficial es `Microsoft.Direct3D.D3D12` / `1.721.3-preview`, en NuGet.

| Archivo | Compatibilidad del instalador | Tamaño | SHA-256 |
|---|---|---:|---|
| `OptiScaler-NR-Publico-B580-SinPesos-Setup.exe` | B580 | 129.352.150 bytes | `a93d77e1d5ceef6485f31932a098a4b04c637dd47f6df1983f1526b018159a6e` |
| `OptiScaler-NR-Publico-B570-B580-SinPesos-Setup.exe` | B580 y B570 experimental | 129.352.147 bytes | `34cd1d3d3a096fc0423bdba61980b8f66d36cb634f760e69a502456374a8c019` |

Cada paquete lleva 46 archivos: DLL de integración, bibliotecas FidelityFX/XeSS procedentes de sus SDKs, licencias/avisos, ajustes, lanzador y documentación. Los archivos de SDK que se aporten localmente se verifican mediante hashes, no se descargan automáticamente. El nombre «cinco modos» se refiere a los modos disponibles cuando el modelo está aportado, no a cinco modelos incluidos en la descarga.

## Instalar OptiScaler base

1. Cerrar el juego y seleccionar su ejecutable real en el instalador.
2. Elegir la DLL proxy apropiada; DXGI es la primera opción.
3. Instalar. NR inicia desactivado y `GlobalB1=0`, por lo que no se intenta crear el dispositivo experimental durante este arranque básico.
4. Iniciar el juego normalmente para usar OptiScaler sin NR. El soporte de reescalado/generación depende del motor y del driver. El lanzamiento real con estos paquetes públicos todavía requiere prueba por juego.

## Aportar NR localmente

Solo si dispones de un paquete compatible obtenido con autorización:

1. Copiar en la carpeta del juego `native_nr_modes/assets-180`, `assets-288`, `assets-360`, `assets-540` y `assets-720`, con sus manifiestos y todos los archivos indicados. No se incluyen ni proporcionan enlaces a esos pesos aquí.
2. Obtener el runtime experimental compatible del proveedor y colocarlo en `native_nr_runtime`, sin reemplazar el D3D12Core original del juego. El contrato incluido identifica por hashes los archivos esperados; otra versión no se considera intercambiable automáticamente.
3. Con el juego cerrado, abrir otra vez el instalador, seleccionar el mismo ejecutable y pulsar **Modelo local**.
4. Se comprueban las cinco geometrías, conteos, rutas, tamaños, hashes y runtime. Si todo es correcto, se actualiza el perfil de arranque y se habilita `GlobalB1=1`. **Enabled sigue a cero.**
5. Ejecutar `Launch-OptiScaler-NR.bat` como administrador, dejar la ventana abierta y activar NR desde Insert → Custom. Empezar por 320×180.

La comprobación sin cambios está disponible con `nr-general-installer/Uninstall.exe --check-model <ejecutable-del-juego>`. Los archivos de modelo/runtime aportados manualmente no los administra el instalador y se conservan al desinstalar. La desinstalación restaura los ajustes/perfil registrados; los cambios posteriores del usuario se conservan y notifican.

## Pruebas y límites

Ambas variantes pasaron un total de 20 comprobaciones: integridad, política B580/B570, instalación, rechazo de modelo ausente o geometría incorrecta, VerifyOnly del lanzador, verificación/registro de un modelo local y desinstalación restaurando el original. El ensayo utilizó paquetes locales ya disponibles; **no se lanzó un juego, no se cambiaron ajustes de Windows ni se volvió a medir inferencia GPU**. Las DLL instaladas en los juegos reales permanecieron intactas.

No significa compatibilidad universal ni validación física de B570. Requiem sigue necesitando su integración específica y está bloqueado por el instalador general.

## Código correspondiente

El motor es `installer/GenericInstaller.cs`, compilado con `PUBLIC_PACKAGE` y, para B580, `B580_ONLY`. `tools/build_public_installers.py` empaqueta mediante allowlist y excluye completamente `native_nr_modes/` y `native_nr_runtime/`. `tools/test_public_installers.py` describe las pruebas aisladas.

El empaquetador conserva entradas del laboratorio local: lee los instaladores completos para obtener DLL/ajustes y contratos de hashes, además de los SDKs oficiales para las bibliotecas redistribuibles. Esos instaladores completos no se publican. Para reconstrucción independiente hay que proporcionar los binarios/configuración equivalentes desde la compilación local y adaptar las rutas; el código del empaquetado y los contratos son públicos, no el material del modelo.
