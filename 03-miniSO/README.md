# 03 - Mini sistema operativo

Ejemplo educativo y simple de varias tareas FreeRTOS funcionando como servicios
de un mini sistema operativo sobre ESP32.

## Servicios

- **Logger:** recibe eventos mediante una cola y los muestra por Serial.
- **Sensor:** genera un valor virtual cada dos segundos.
- **Comunicaciones:** simula un enlace activo.
- **Estadisticas:** informa heap, tiempo, tareas, prioridad, stack libre y core.
- **Watchdog:** avisa si un servicio deja de actualizar su actividad.
- **Consola:** interpreta comandos escritos en el monitor serie.

## Uso

Abrir el monitor serie a `115200 baudios` y escribir un comando seguido de Enter:

```text
help
tasks
status
pause sensor
resume sensor
priority logger 5
```

El reporte automatico aparece cada cinco segundos. La cantidad de stack libre
se expresa en palabras de FreeRTOS, no en bytes.