# OptiScaler NR para Intel Arc

Integración experimental de reconstrucción neuronal en OptiScaler para Windows/D3D12. La referencia de hardware es **Intel Arc B580**; **B570 se admite de forma experimental, sin prueba física**. Spider-Man 2 es la referencia de la ruta general. Resident Evil Requiem empleó una integración y un lanzador específicos.

Este es un port de investigación basado en un modelo recuperado por terceros. No es una versión oficial de NVIDIA DLSS, Intel XeSS ni OptiScaler. No hemos entrenado el modelo original y no afirmamos equivalencia con el producto de NVIDIA.

## Estado de la publicación

Se publica el código de la ruta nativa, shaders propios, integración, instalador y documentación. **No se publican pesos, paquetes de modelo, DLL NVIDIA, capturas de juegos ni instaladores completos que los contengan.** Los [dos instaladores públicos sin pesos ni SDK experimental](docs/INSTALADORES-PUBLICOS.md) están disponibles en [Releases](https://github.com/byroncalle23/optiscaler-nr-intel-arc/releases/tag/v1.2.0-public-no-weights). Instalan OptiScaler y requieren aportar/verificar el modelo y runtime localmente para activar NR. Los paquetes locales completos siguen excluidos.

La ruta admite elegir un ejecutable DX12, pero **no está validada en cualquier juego**. El temporal depende de que se capturen recursos, estados y metadatos correctos del motor. El instalador general rechaza Requiem para conservar su integración específica.

## Lo conseguido

- Grafo Global B1: una evaluación por fotograma, sobre una entrada compacta.
- Inferencia y composición en GPU; la imagen nativa recibe el residual ampliado con EdgeGuidedResidual.
- Cola DIRECT y sincronización por recursos/fences. La ejecución síncrona no significa esperar en CPU cada fotograma.
- Historial, movimiento y profundidad a través del puente del motor cuando la ruta está verificada.
- Menú con cinco geometrías y reinicio seguro del historial al cambiar de modo.
- Instalación con respaldo, rollback, desinstalación y pesos compartidos mediante enlaces NTFS en el paquete local.

| Modo de entrada | Campo con padding | Tokens del bottleneck | Mediana GPU aislada* |
|---|---|---:|---:|
| 320×180 | 320×192 | 32 | 11,86 ms |
| 512×288 | 512×320 | 64 | 22,78 ms |
| 640×360 | 640×384 | 96 | 31,71 ms |
| 960×540 | 960×576 | 192 | 67,58 ms |
| 1280×720 | 1280×768 | 240 | 113,74 ms |

*Salida 1440p, fixture histórico de Requiem, temporal y bordes activos; 40 fotogramas, 8 de calentamiento y 32 muestras. No son FPS medidos en Spider-Man ni promesas de rendimiento en otros equipos. Son resultados de una etapa anterior al ajuste final del instalador; no se volvieron a medir al añadir B570.

## Documentación

1. [Proceso de trabajo y decisiones](docs/FLUJO-DE-TRABAJO.md).
2. [Arquitectura, sincronización y temporal](docs/ARQUITECTURA-WINDOWS.md).
3. [Instalación y diagnóstico](docs/INSTALACION.md).
4. [Compilación y reproducción](docs/COMPILACION.md).
5. [Cómo continuar y mejorar](docs/CONTINUAR-DESARROLLO.md).
6. [Artefactos, verificaciones y límites](docs/ARTEFACTOS.md).
7. [Procedencia y licencias](NOTICE.md).
8. [Instaladores públicos: contenido, aporte local y hashes](docs/INSTALADORES-PUBLICOS.md).

Los ficheros de `src/native` incluyen ensayos descartados además de producción. **No activar todos los defines experimentales**: el script de compilación fija la política aceptada. Los resultados históricos no prueban que todo el directorio esté validado para publicación o despliegue en juego.
