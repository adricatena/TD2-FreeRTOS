# 03 - Mini sistema operativo

Ejemplo educativo y simple de varias tareas FreeRTOS funcionando como servicios
de un mini sistema operativo sobre ESP32.

## Servicios

- **Logger:** recibe eventos mediante una cola y los consume sin imprimirlos automaticamente.
- **Sensor:** genera un valor virtual cada dos segundos.
- **Comunicaciones:** simula un enlace activo.
- **Estadisticas:** mantiene activa la tarea y permite consultar el estado bajo demanda.
- **Watchdog:** controla si los servicios siguen actualizando su actividad.
- **Consola:** interpreta comandos escritos en el monitor serie.

## Uso

Abrir el monitor serie a `115200 baudios`, escribir un comando y presionar Enter.
La consola no muestra reportes automaticamente; responde solamente cuando se
ingresa un comando.

### Comandos

```text
help
report
tasks
status
pause sensor
resume sensor
priority logger 5
```

- `help`: muestra la lista de comandos disponibles.
- `report`: muestra el reporte completo del sistema: heap libre, tiempo de
	ejecucion y stack libre de cada tarea.
- `tasks`: muestra la prioridad y el stack libre de cada tarea.
- `status`: muestra si el sensor esta activo o pausado, su valor actual y el
	heap libre.
- `pause sensor`: suspende la tarea del sensor. Su valor deja de actualizarse.
- `resume sensor`: reanuda la tarea del sensor.
- `priority logger <numero>`: cambia la prioridad de `Logger`. El numero debe
	estar dentro del rango permitido por FreeRTOS.

Por ejemplo:

```text
priority logger 5
report
```

La cantidad de stack libre se expresa en palabras de FreeRTOS, no en bytes.