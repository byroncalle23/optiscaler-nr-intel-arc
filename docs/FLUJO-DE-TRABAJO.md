# Proceso y decisiones

## 1. Recuperación y referencia

Se partió del trabajo independiente de recuperación del grafo y los pesos, no de entrenamiento propio. El proyecto original de Uzbekunknown implementó una ruta Vulkan/Linux en una integrada Xe2; el trabajo Windows/B580 desarrolló otra ejecución e integración. La arquitectura lógica recuperada ronda 145,8 millones de parámetros; contar parámetros no determina por sí solo las operaciones ni los milisegundos de una inferencia.

Se estableció una referencia numérica y un banco de pruebas fuera del juego. Las lecturas de salida para comparar resultados se hicieron después del fence. Esa instrumentación de diagnóstico no forma parte del objetivo de inferencia sin round-trips CPU por fotograma.

## 2. D3D12/DirectML y mosaicos

Se implementaron tensores residentes, preparación única, asignación/scratch, barreras, despacho y composición. El primer esquema cubría 1080p con 45 mosaicos; se exploraron batches B2/B4, pesos compartidos y aislamiento de atención. Agrupar mosaicos no equivale a dividir el coste por el batch: permanecen cálculo repetido, memoria y sincronización.

La reutilización de command lists redujo el coste de grabación de CPU en la ruta medida. No se deben reutilizar allocators ni modificar descriptores/recursos mientras la GPU los use. La lista debe reconstruirse ante cambios incompatibles de recursos, formato o geometría. D3D12 bundles no son sustitutos generales de un command list de cómputo.

## 3. Ruta matricial nativa

Se probaron el SDK experimental 721 y Shader Model 6.10, consultas de capacidad y multiplicación Wave FP16. Se migraron proyecciones constantes y segmentos con shaders nativos y se compararon contra la referencia. DirectML permanece en partes de la implementación; **el grafo no se presenta como totalmente libre de DirectML**.

Un operador INT8 lento no demuestra por sí solo que DirectML jamás utilice DPAS. La evidencia válida debe proceder de perfiles de la ruta concreta. Tampoco se puede identificar todo cálculo FP16 con XMX: son unidades y rutas distintas.

## 4. Paso global

El salto mayor vino de cambiar el área de inferencia, no de otro epílogo. En una comparación aislada, la ruta de mosaicos consumió aproximadamente 1.119 ms; Global B1 a 640×360 bajó a 53,73 ms y a 960×540 a 115,40 ms. Incluían reducción, grafo y composición, sin preparación inicial ni readback.

Se reduce el color de entrada; el modelo procesa el campo global con padding y se obtiene un residual de baja resolución. Se amplía ese residual sobre la imagen nativa, no toda la imagen reducida. La aritmética no escala simplemente con el número de tokens: las etapas de alta resolución y los anchos de canal también cuentan.

## 5. Perfil y fusiones aceptadas

La política aceptada combina partición de atención WG256, NormWave8, SoftmaxWave8, DenseN32 y WindowSegmentFused. El grafo 360p alcanzó aproximadamente 31 ms en el banco usado. Ensayos de INT8 dinámico, epílogos, variantes de registros y otros kernels se descartaron cuando no mejoraron el fotograma completo o perjudicaron calidad.

La regla del proyecto pasó a ser: priorizar bloques de al menos 3 ms y exigir al menos 0,5 ms de ganancia neta en el fotograma completo. Es una decisión de enfoque, no una ley del hardware.

## 6. Calidad temporal y bordes

Las primeras versiones presentaban parpadeos, halos y estelas. Se separó el efecto de NR del reescalado mediante pruebas activado/desactivado, AA nativo y vídeos aportados por el usuario. Se corrigieron captura/avance del historial, puentes de movimiento/profundidad y composición del residual en bordes. El usuario informó reducción de parpadeo y conservación del efecto; esto no significa que todos los artefactos estén eliminados.

La referencia EdgeGuidedResidual se conservó con SHA que empieza EE2EF23B. No se sustituyó como referencia por cada experimento de rendimiento.

## 7. Compactación y Spider-Man 2

Se probó reducir la geometría a 512×288 y después 320×180. El núcleo del modelo siguió completo; no hubo entrenamiento ni inferencia alterna. La desviación geométrica frente a 360p superó el 2,5% en ciertos ensayos, pero el usuario autorizó excepciones visuales reversibles y aceptó el balance de rendimiento de los modos compactos.

Spider-Man 2 mostró que reutilizar la ruta de un juego no bastaba: hubo que corregir la identificación del dispositivo/recurso y la captura de profundidad planar para conseguir el temporal. El usuario confirmó que las estelas y parpadeos observados desaparecieron en esa prueba tras aplicarlo. Otros juegos necesitan sus propias verificaciones.

## 8. Cinco modos e instalador

Se añadieron cinco geometrías, preparación por generación, retiro de la sesión anterior, historial limpio y control del presupuesto VRAM. Los modos 540p/720p pueden exceder memoria disponible o ser demasiado lentos. Se corrigieron rechazos y grabaciones pendientes durante transiciones; no se afirma validación exhaustiva de todos los cambios en vivo.

El instalador general selecciona el ejecutable y el nombre de la DLL proxy, respalda, verifica hashes y restaura. Se compiló una variante B570/B580 con detección PCI exacta; B570 no tuvo prueba física. Las comprobaciones del instalador no sustituyen una sesión real de juego.
