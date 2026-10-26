# 04 - Servidor web y diagnostico del RTOS

Este proyecto crea un punto de acceso Wi-Fi con el nombre `ESP32-RTOS` y la clave `freertos`.
El celular debe conectarse a esa red y abrir `http://192.168.4.1`.

Si el chip incluye sensor Hall interno, la tarea `Sensor` queda fijada al ultimo nucleo disponible y lo lee. La red y el servidor
asincrono quedan gestionados por el Core 0. El panel actualiza cada segundo:

- uso estimado de CPU de cada nucleo;
- heap libre actual, minimo y bloque contiguo mas grande;
- lectura actual, minimo, maximo y promedio del sensor Hall, cuando el chip lo incluye;
- tiempo activo;
- cantidad de dispositivos conectados al punto de acceso;
- IP, canal Wi-Fi, cantidad de tareas y datos basicos del chip;
- high water mark de la pila de las tareas educativas.

## Observaciones didacticas

- El porcentaje de CPU es una estimacion educativa: una tarea de cada nucleo mide su capacidad de trabajo y la compara con una referencia.
- No es un profiler exacto. Sirve para observar como el Wi-Fi, el servidor y las tareas compiten por tiempo de CPU.
- El high water mark es la minima cantidad historica de palabras de pila libres.
- El servidor responde sin que la tarea del sensor tenga que atender clientes ni esperar por la red.
- En variantes sin sensor Hall (por ejemplo ESP32-S2, ESP32-C3, ESP32-S3 y ESP32-C6), el proyecto no
  compila ni ejecuta llamadas a `hallRead()`, y oculta del panel las lecturas, estadisticas y pila de esa tarea.

Para cargarlo desde PlatformIO, abrir esta carpeta como proyecto y usar `Upload`.