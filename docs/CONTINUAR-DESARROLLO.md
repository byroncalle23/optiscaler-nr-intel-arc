# Cómo continuar y mejorar

## Prioridad 1: compatibilidad y diagnóstico

Registrar por juego API, entrada de reescalado, formato SDR, dispositivo nativo, origen de depth/MV, escalas, jitter, estados y momento del callback. Implementar perfiles explícitos cuando el contrato no sea generalizable. No ocultar un fallo temporal con la ruta anterior y declarar que es compatible.

Añadir pruebas sintéticas de reproyección: movimiento de un píxel, cámara quieta, objeto que tapa/revela otro, cambio de profundidad, cambio de jitter y reset. La calidad requiere además vídeo A/B en escena real. Medir rechazo de historial, disoclusiones y error temporal, no solo RMSE de una imagen.

## Prioridad 2: medición limpia del fotograma

Fijar escena, geometría, driver, resolución de salida y política. Calentar el grafo; medir timestamps GPU para puente, reducción, atención, GEMMs, FFN y composición, junto a CPU de grabación y presentación. Separar preparación inicial y capturas de diagnóstico. Reportar mediana y dispersión, con el juego cerrado para el banco y con carga del motor para validación real.

PIX/Intel GPA pueden aportar ISA, DPAS y presión de registros según soporte del driver/herramienta. No prometer siempre acceso a cada contador ni inferir spills solo de lentitud. Comparar variantes A/B alternadas en la misma sesión y aceptar por ahorro del pipeline completo.

El criterio de enfoque vigente: bloque agregado de al menos **3 ms** y ganancia potencial/neta de al menos **0,5 ms**. Evitar volver a epílogos de décimas salvo que corrijan un fallo funcional.

## Prioridad 3: arquitectura para reducir trabajo

La compactación ya dio más que muchas microoptimizaciones, con pérdida visual aceptada por el usuario. Explorar campos y ruido coherentes con el modelo, medir calidad en movimiento y documentar desviaciones. El límite de 2,5% relativo RGB sigue siendo la referencia general; 512/320 fueron excepciones visuales, no prueba de que cumplan ese límite.

Alternar inferencia podría bajar coste promedio, pero un frame de 31 ms y otro de 0,4 ms no garantizan ritmo constante ni 60 FPS. Requiere reproyección robusta, recuperación ante disoclusiones y medir percentiles de frame time. No está implementado como política de producción.

## Prioridad 4: kernels matriciales y segmentos

Atacar GEMMs/FFN/atención dominantes con reutilización de pesos, formatos compactos emitidos por productores y trabajo cooperativo multi-fila. Verificar capacidad Wave/ThreadGroup real, formas y tipos de acumulación. Incluir coste de empaquetar matrices dinámicas K/V y la presión de memoria en el resultado, no solo el kernel.

La ruta híbrida todavía puede pagar conversiones y barreras. Sustituir DirectML no es una garantía automática: primero comparar un segmento completo. Mantener aislamiento entre mosaicos/batches y distinguir GEMMs con pesos constantes de atención con dos operandos dinámicos.

INT8 dinámico en canales pequeños fue descartado por escalas/packing y regresión global. QAT/PTQ sería otra investigación con datos representativos y segmentos encadenados; no repetir solo un kernel rápido y anunciar 3× en todo el grafo.

## Prioridad 5: un modelo más pequeño

Para un salto mayor puede diseñarse un alumno: reducir ancho/profundidad, destilar salida y pérdidas perceptuales/temporales del maestro y entrenar con escenas variadas. Entrenar el mismo grafo en B570 en lugar de B580 no lo hace más eficiente; la mejora debe venir de arquitectura, objetivo o formato de ejecución.

Requiere derechos sobre pesos y datos, conjunto de entrenamiento separado de pruebas y una referencia reproducible. En este proyecto no se realizó ese entrenamiento. No se dispone aquí de un checkpoint alumno ni una receta validada de entrenamiento.

## Calidad y contribuciones

Conservar una referencia inmutable, hashes, configuración y rollback por experimento. Una propuesta debe incluir problema, código, protocolo A/B, ahorro neto, RMSE relativo definido y evaluación temporal. RMSE relativo: `sqrt(mean((actual-ref)^2)) / max(sqrt(mean(ref^2)), epsilon)` sobre RGB con espacio/color y composición documentados.

No afirmar que 2,5% es un estándar universal de la industria: es el umbral acordado para este proyecto. No introducir pesos ni capturas privadas en commits. Abrir issues con logs redactados, formatos y escena descrita; vídeos/imágenes solo cuando su publicación esté autorizada.
