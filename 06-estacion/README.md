# 06 - Estacion de monitoreo y control

Este proyecto propone construir progresivamente una pequeña estación de
monitoreo y control sobre un ESP32 usando FreeRTOS. La estación simula sensores
ambientales y actuadores, pero su arquitectura permite reemplazar las
mediciones simuladas por hardware real.

El objetivo es integrar tareas, retardos, colas, mutexes, prioridades,
temporizadores, notificaciones, grupos de eventos, semáforos, watchdog,
diagnóstico y una interfaz de comandos. La interfaz web es opcional y debe
incorporarse después de que el núcleo de FreeRTOS funcione.


## Modelo del sistema

```text
SensorTask ──► measurementQueue ──► ControlTask ──► actuadores
                                      │
                                      ├──► eventQueue ──► AlarmTask
                                      │
Consola/Web ──► commandQueue ────────┘

DiagnosticsTask ──► estado, colas, heap, stack y actividad
WatchdogTask    ──► supervisión de las demás tareas
```

## Estado final esperado

La estación debería poder:

- generar o leer temperatura, humedad y nivel de agua;
- procesar las mediciones en una tarea de control;
- encender y apagar ventilador, bomba y luz;
- activar una alarma ante condiciones peligrosas;
- aceptar comandos desde la consola serie;
- mantener un estado compartido protegido por mutex;
- comunicar mediciones, comandos y eventos mediante colas;
- utilizar prioridades justificadas;
- apagar una bomba mediante un temporizador;
- despertar tareas mediante notificaciones;
- esperar condiciones de inicialización con un grupo de eventos;
- representar una interrupción o recurso disponible con un semáforo;
- detectar tareas que dejaron de actualizar su actividad;
- mostrar métricas de heap, stack, colas y eventos descartados;
- opcionalmente exponer el estado por HTTP o WebSocket.

## Cómo ejecutar el punto de partida

1. Abrir esta carpeta como proyecto PlatformIO.
2. Cargar el firmware en un ESP32.
3. Abrir el monitor serie a `115200 baudios`.
4. Observar una medición cada dos segundos.
5. Observar el estado de los actuadores y el diagnóstico periódico.

La primera versión no contiene todavía la solución completa. Los `TODO` y los
comentarios de `src/main.cpp` marcan los puntos que se implementan durante las
etapas.

## Desarrollo etapa por etapa

### Etapa 1 - Tareas periódicas y mediciones

**Objetivo:** crear una tarea que genere una medición con una frecuencia
definida y otra que la procese.

Trabajar:

- `xTaskCreate`;
- `vTaskDelay`;
- período de una tarea;
- diferencia entre una tarea periódica y un bucle ocupado;
- valores simulados frente a sensores reales.

Resultado esperado: `SensorTask` produce una medición y `ControlTask` recibe
una medición. Todavía puede utilizarse una variable simple para comprender el
flujo, pero la versión recomendada utiliza una cola desde el principio.

### Etapa 2 - Cola productor-consumidor

**Objetivo:** separar la generación de datos de su procesamiento.

Implementar:

- `measurementQueue`;
- envío de una copia de `Measurement`;
- recepción bloqueante en `ControlTask`;
- política ante cola llena;
- contador de mediciones descartadas.


### Etapa 3 - Control automático y estado compartido

**Objetivo:** convertir mediciones en decisiones sobre actuadores.

Reglas iniciales sugeridas:

```text
temperatura >= 30 °C  -> ventilador encendido
nivel de agua <= 25 %  -> bomba encendida
modo manual            -> no modificar actuadores automáticamente
```

Agregar histéresis para evitar conmutaciones excesivas:

```text
encender ventilador a 30 °C
apagar ventilador a 28 °C
```

El estado global debe protegerse con `stateMutex`. Toda tarea que lo lea o
modifique debe utilizar el mismo mutex.

### Etapa 4 - Consola de comandos

**Objetivo:** aceptar órdenes sin acoplar la consola con el control.

Comandos sugeridos:

```text
help
status
auto on
auto off
fan on
fan off
pump on
pump off
light on
light off
alarm reset
```

`CommandTask` debe validar y publicar comandos en `commandQueue`. El control
debe aplicar las órdenes. La consola no debe modificar directamente los
actuadores.

### Etapa 5 - Eventos y alarmas

**Objetivo:** separar una condición anormal de la acción que la atiende.

Eventos sugeridos:

```text
TEMPERATURE_HIGH
WATER_LOW
SENSOR_FAILURE
EMERGENCY_STOP
```

`ControlTask` publica eventos en `eventQueue`. `AlarmTask` los consume y
actualiza la salida de alarma. Definir qué ocurre cuando la cola de eventos
está llena y cómo se informa el error.

### Etapa 6 - Prioridades y comportamiento temporal

**Objetivo:** justificar las prioridades en lugar de asignarlas al azar.

Una propuesta inicial:

```text
AlarmTask       prioridad alta
ControlTask     prioridad media-alta
SensorTask      prioridad media
CommandTask     prioridad baja
DiagnosticsTask prioridad baja
```

Probar qué sucede si una tarea consume demasiado tiempo o tiene una prioridad
innecesariamente alta. Relacionar el resultado con starvation, latencia y
capacidad de respuesta.

### Etapa 7 - Temporizadores de software

**Objetivo:** ejecutar una acción diferida sin crear una tarea para cada
temporización.

Ejemplos:

- apagar la bomba después de 10 segundos;
- hacer parpadear la alarma;
- generar un evento si una medición no llega a tiempo;
- volver al modo seguro después de un período sin comandos.

Comparar un `TimerHandle_t` con `vTaskDelay`: el temporizador notifica una
acción, mientras que `vTaskDelay` suspende el flujo de una tarea.

### Etapa 8 - Notificaciones directas

**Objetivo:** usar una señal liviana entre tareas.

Usar una notificación para:

- despertar `AlarmTask` ante una emergencia;
- informar que un actuador terminó una operación;
- despertar una tarea de mantenimiento.

Comparar notificación, cola y semáforo:

| Mecanismo | Transporta datos | Destino |
|---|---:|---|
| Cola | Sí | Una o varias tareas |
| Notificación | Valor o bits | Una tarea |
| Semáforo | No | Sincronización |

### Etapa 9 - Grupos de eventos

**Objetivo:** representar condiciones globales del sistema.

Bits sugeridos:

```text
BIT_SENSOR_READY
BIT_CONTROL_READY
BIT_NETWORK_READY
BIT_EMERGENCY_STOP
```

La tarea de control puede esperar a que el sistema esté inicializado. Una
parada de emergencia puede bloquear acciones normales hasta ser reconocida.

### Etapa 10 - Semáforos

**Objetivo:** distinguir exclusión mutua, sincronización y disponibilidad.

Usos posibles:

- semáforo binario liberado por una interrupción;
- señal de que un ADC terminó una conversión;
- permiso para usar un recurso de hardware;
- sincronización de una etapa de inicialización.

Explicar por qué el mutex del estado no debe reemplazarse automáticamente por
un semáforo binario: tienen intenciones distintas y el mutex ofrece herencia
de prioridad.

### Etapa 11 - Watchdog y supervisión

**Objetivo:** detectar que una tarea dejó de funcionar aunque el sistema no
se haya reiniciado todavía.

Cada tarea debe registrar su última actividad. `WatchdogTask` debe comprobar:

- antigüedad de cada heartbeat;
- colas permanentemente llenas;
- sensores sin mediciones;
- tareas suspendidas o bloqueadas demasiado tiempo.

Luego se puede integrar el watchdog de hardware como extensión, pero primero
conviene construir una supervisión observable y educativa.

### Etapa 12 - Diagnóstico

**Objetivo:** hacer medible el comportamiento del sistema.

Mostrar:

- heap libre y mínimo histórico;
- high water mark de cada tarea;
- elementos pendientes de cada cola;
- espacios disponibles;
- mediciones recibidas;
- eventos publicados y descartados;
- comandos aceptados y rechazados;
- tiempo activo;
- estado de actuadores.

No presentar una métrica sin explicar qué decisión permite tomar.

### Etapa 13 - Interfaz web opcional

**Objetivo:** agregar una interfaz sin alterar la arquitectura de FreeRTOS.

La web debe ser otro productor de comandos y otro consumidor de un snapshot,
no una segunda implementación del control.

Opciones:

- HTTP con polling, más simple para la clase;
- WebSocket, como extensión de comunicaciones asincrónicas.

La lógica de control debe continuar funcionando aunque no haya clientes
conectados.

## Criterios de finalización

El proyecto puede considerarse integrado cuando:

- cada tarea tiene una responsabilidad clara;
- los datos cruzan contextos mediante primitivas apropiadas;
- no hay punteros a buffers temporales en colas;
- el estado compartido está protegido;
- los errores y descartes son observables;
- las prioridades tienen una justificación;
- las tareas tienen stack suficiente;
- la estación entra en un estado seguro ante fallos;
- el sistema puede diagnosticarse sin detenerlo.
