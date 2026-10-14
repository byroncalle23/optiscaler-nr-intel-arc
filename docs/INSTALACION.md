# Instalación y diagnóstico

## Disponibilidad

Los instaladores locales completos incluyen pesos y no están publicados en este repositorio. La fuente del instalador permite estudiar y reconstruir el mecanismo. Cualquier variante sin pesos requiere un paquete de modelo local obtenido con autorización; **no genera ni descarga automáticamente los pesos**.

## Requisitos de la ruta NR

- Windows x64, juego D3D12 y salida SDR.
- B580 probada como referencia; B570 experimental sin medición propia.
- Driver y runtime que expongan Shader Model 6.10 y las formas matriciales FP16 requeridas.
- Dependencias OptiScaler y runtime privado compatible. La versión de driver usada durante el trabajo local fue 9033; no se afirma que sea requisito mínimo.
- Memoria disponible suficiente. Empezar por 320×180.

## Uso del paquete local completo

1. Cerrar el proceso real del juego, no solo su ventana.
2. Seleccionar el ejecutable que renderiza el juego; en Unreal puede ser el ejecutable Shipping y no el lanzador de la raíz.
3. Probar DXGI como DLL proxy inicial. `version.dll` y `winmm.dll` son alternativas; no instalar varias copias de OptiScaler en paralelo.
4. El instalador respalda archivos afectados y conserva ajustes según el manifiesto. En Spider-Man se adapta `Dxgi=false`; no extrapolar ese ajuste a todos los motores.
5. Iniciar `Launch-OptiScaler-NR.bat` como administrador y dejar su ventana abierta. El helper gestiona temporalmente el modo de desarrollador del SDK experimental y lo restaura al finalizar normalmente. Una terminación forzada del helper puede impedir ejecutar su bloque de restauración.
6. Insert → Custom → activar reconstrucción neuronal → modo 320×180. Comparar contra NR desactivado con los demás ajustes iguales.
7. Verificar el estado temporal. «Pase enviado» indica envío, no prueba por sí solo que la calidad temporal sea correcta.

Cerrar el juego y ejecutar `Desinstalar-OptiScaler-NR.bat` para restaurar. Los archivos modificados por el usuario después de instalar se conservan y se notifican; revisar el registro de desinstalación antes de borrar nada manualmente.

## Prueba de otro juego

Empezar con frame generation desactivado. Grabar una escena quieta y el mismo recorrido con NR apagado/encendido: cámara, movimiento del personaje, apuntado, rejillas, cabello, luces y disoclusiones. Probar primero una ruta de entrada cuyo puente sea identificable. En Spider-Man se utilizó FSR 3.1.5 como referencia; no es una regla universal.

Si el temporal no se aplica, revisar el log y corregir el puente del motor antes de compensarlo aumentando la mezcla. Un juego puede arrancar con OptiScaler y no tener NR compatible.

## Fallos comunes

| Síntoma | Comprobación |
|---|---|
| Error al preparar NR | Ruta/manifiesto del modelo, hashes, SDK, SM6.10 y presupuesto VRAM |
| Temporal no aplicado | Dispositivo nativo, formato/plano depth, estados, metadatos y asociación de frame |
| Parpadeo/estelas | A/B sin FG, AA nativo, movimiento, historial válido y avance de frame |
| Modo alto rechazado | Memoria ya ocupada por el juego; volver a modo bajo |
| No se puede sustituir DLL | Proceso del juego todavía activo |
| Cambió el ejecutable del juego | Revisar versión y regenerar perfil verificado; no eliminar comprobaciones a ciegas |

Requiem mantiene un lanzador y una integración específicos con REFramework; el instalador general lo rechaza. No se incluyen sus parches de compatibilidad en la publicación general.
