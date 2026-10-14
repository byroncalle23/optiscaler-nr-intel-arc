# Artefactos y evidencia

## Instaladores locales completos, no publicados

| Variante | SHA-256 | Tamaño |
|---|---|---:|
| General B580 | `c57e6016c3221fb88feec8e83506bc301ea0dccba848b810fb1f99a8734fb8b4` | 320.296.455 bytes |
| General B570/B580 | `73154d0fa4dd3be2aeb09c262994cc7faee1288271fa94c5e4f85d2d09194334` | 320.296.606 bytes |

Son paquetes de uso local que contienen pesos. Sus hashes identifican lo probado; no son enlaces de descarga ni autorizaciones de redistribución. El paquete particular de Requiem es distinto y no debe confundirse con estos dos generales.

## DLLs de referencia

- B580 cinco modos E36: `E36E447DF8CE4134DFB1D75AC8A6537A98D0FA78CB8338A9F69749BC6CE0887D`.
- B570/B580: `249B08D43612737ABA66ED462CA4F5C5F98D9764512EBF35836D322C6092E16F`.
- Referencia de calidad EdgeGuidedResidual: `EE2EF23BD0423B7E57B866A2C6D44770D2EE5FD1CE01052CA78A7D6B1FD977E6`.

## Pruebas del instalador

El instalador B580 pasó una suite de 15 comprobaciones; el B570/B580 pasó 20 al añadir casos PCI. Se verificaron integridad, instalación en ruta Unicode, lanzador en VerifyOnly sin cambiar Windows/abrir juego, pesos compartidos NTFS, instalación duplicada, preservación de cambios, desinstalación, path traversal, blob alterado, colisión de wrapper, rollback, carga alternativa y restauración, perfil específico de Requiem y ejecutable inválido. Una prueba adicional del primer instalador rechazó un ejecutable de prueba abierto.

La DLL B570/B580 conserva 959 pares de nombre/ordinal de exportación respecto a E36. En esa adaptación no se volvió a medir inferencia GPU ni se probó una B570 física. Las instalaciones reales de Spider-Man y Requiem no se modificaron durante el empaquetado general.

## Interpretar los tiempos

Un juego a 65 FPS tarda aproximadamente 15,38 ms por frame; a 25 FPS, 40 ms. La diferencia observada es 24,62 ms, pero no aísla por sí sola el kernel NR: hay carga compartida, memoria y sincronización.

Perder como máximo 40% desde 65 FPS significa al menos 39 FPS (25,64 ms). Bajo un modelo aditivo sencillo deja aproximadamente 10,26 ms adicionales. La medición aislada 320p de unos 11,86 ms **no demuestra cumplir esa meta en juego**.

## Estado de validación

| Ámbito | Evidencia | Pendiente |
|---|---|---|
| B580/Requiem | Banco, vídeos y veredictos del usuario | Compatibilidad universal y ausencia total de artefactos |
| B580/Spider-Man 2 | Temporal y compacto aceptados en prueba del usuario | Todos los modos altos y transiciones exhaustivas |
| B570 | Compilación y identificación de software | Arranque, capacidades reales, imagen, memoria y rendimiento |
| Otros juegos | Instalador genérico | Contratos de recursos y pruebas reales |
| Publicación binaria | Artefactos locales identificados | Paquetes sin pesos y distribución de dependencias verificadas |
