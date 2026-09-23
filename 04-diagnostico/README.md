# 04 - Servidor web y diagnostico del RTOS

Este proyecto crea un punto de acceso Wi-Fi con el nombre `ESP32-RTOS` y la clave `freertos`.
El celular debe conectarse a esa red y abrir `http://192.168.4.1`.

La tarea `Sensor` queda fijada al Core 1 y lee el sensor Hall interno. La red y el servidor
asincrono quedan gestionados por el Core 0. El panel actualiza cada segundo:

- uso estimado de CPU de cada nucleo;
- heap libre actual, minimo y bloque contiguo mas grande;
- lectura actual, minimo, maximo y promedio del sensor Hall;
- tiempo activo;
- cantidad de dispositivos conectados al punto de acceso;
- IP, canal Wi-Fi, cantidad de tareas y datos basicos del chip;
- high water mark de la pila de las tareas educativas.

## Observaciones didacticas

- El porcentaje de CPU es una estimacion educativa: una tarea de cada nucleo mide su capacidad de trabajo y la compara con una referencia.
- No es un profiler exacto. Sirve para observar como el Wi-Fi, el servidor y las tareas compiten por tiempo de CPU.
- El high water mark es la minima cantidad historica de palabras de pila libres.
- El servidor responde sin que la tarea del sensor tenga que atender clientes ni esperar por la red.

Para cargarlo desde PlatformIO, abrir esta carpeta como proyecto y usar `Upload`.