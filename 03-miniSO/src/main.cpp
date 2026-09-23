#include <Arduino.h>
#include "esp_system.h"

// Cada tarea representa un servicio del mini sistema.
constexpr uint8_t TASK_COUNT = 6;
// Periodo del reporte automatico de estadisticas.
constexpr TickType_t REPORT_PERIOD = pdMS_TO_TICKS(5000);
// Periodo entre comprobaciones del watchdog.
constexpr TickType_t WATCHDOG_PERIOD = pdMS_TO_TICKS(3000);
// Tamano maximo de un mensaje de evento.
constexpr size_t EVENT_TEXT_SIZE = 64;

// Mensaje que viaja desde los servicios hacia el Logger.
struct Event {
  char text[EVENT_TEXT_SIZE];
};

// Cola compartida por las tareas que generan eventos.
QueueHandle_t eventQueue;
// Evita que dos tareas escriban en Serial al mismo tiempo.
SemaphoreHandle_t serialMutex;
// Handle de cada tarea para consultarla o controlarla.
TaskHandle_t taskHandles[TASK_COUNT] = {};
// Contador de actividad usado por el Watchdog.
volatile uint32_t heartbeats[TASK_COUNT] = {};
// Valor generado por el sensor virtual.
volatile int sensorValue = 0;

// Indices usados para acceder a los servicios por nombre.
enum Service : uint8_t {
  LOGGER,
  SENSOR,
  COMMUNICATIONS,
  STATISTICS,
  WATCHDOG,
  CONSOLE
};

// Imprime una linea protegiendo el puerto serie con un mutex.
void printLine(const char *text) {
  // Espera como maximo 100 ms para obtener el puerto serie.
  if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Serial.println(text);
    // Libera el puerto para otra tarea.
    xSemaphoreGive(serialMutex);
  }
}

// Copia un texto y lo agrega a la cola de eventos.
void logEvent(const char *text) {
  Event event = {};
  // snprintf evita escribir mas alla del buffer del evento.
  snprintf(event.text, EVENT_TEXT_SIZE, "%s", text);
  // No se bloquea: si la cola esta llena, el evento se descarta.
  xQueueSend(eventQueue, &event, 0);
}

// Consume eventos y los muestra ordenadamente por Serial.
void loggerTask(void *parameter) {
  Event event;

  while (true) {
    // Espera hasta que otro servicio publique un evento.
    if (xQueueReceive(eventQueue, &event, portMAX_DELAY) == pdTRUE) {
      // Solo el Logger escribe estos eventos en el puerto serie.
      if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        Serial.printf("[LOG] %lu s - %s\n", millis() / 1000UL, event.text);
        xSemaphoreGive(serialMutex);
      }
    }
    // Informa al Watchdog que el Logger sigue ejecutandose.
    heartbeats[LOGGER]++;
  }
}

// Simula un sensor que cambia su valor periodicamente.
void sensorTask(void *parameter) {
  while (true) {
    // Genera una secuencia simple de valores entre 0 y 99.
    sensorValue = (sensorValue + 7) % 100;
    // Marca una ejecucion correcta del Sensor.
    heartbeats[SENSOR]++;

    char message[EVENT_TEXT_SIZE];
    // Publica el valor generado para que lo muestre el Logger.
    snprintf(message, sizeof(message), "Sensor virtual: valor = %d", sensorValue);
    logEvent(message);
    // Libera el CPU hasta la proxima lectura simulada.
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// Simula una tarea que mantiene un enlace de comunicaciones activo.
void communicationsTask(void *parameter) {
  while (true) {
    heartbeats[COMMUNICATIONS]++;
    logEvent("Comunicaciones: enlace simulado activo");
    // Simula el intervalo entre transmisiones.
    vTaskDelay(pdMS_TO_TICKS(4000));
  }
}

// Muestra recursos y estado de las tareas del sistema.
void statisticsTask(void *parameter) {
  while (true) {
    // El mutex evita mezclar el reporte con mensajes de otras tareas.
    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
      Serial.println("\n==================");
      Serial.println("CPU: tareas FreeRTOS activas");
      // ESP.getFreeHeap devuelve la memoria dinamica disponible.
      Serial.printf("Heap libre: %u bytes\n", ESP.getFreeHeap());
      // millis permite observar cuanto lleva funcionando el sistema.
      Serial.printf("Tiempo de ejecucion: %lu ms\n", millis());
      Serial.println("Stack libre por Task (palabras):");

      for (uint8_t service = LOGGER; service < TASK_COUNT; service++) {
        TaskStatus_t taskStatus;
        // Obtiene prioridad, stack restante y core de una tarea.
        vTaskGetInfo(taskHandles[service], &taskStatus, pdTRUE, eInvalid);
        Serial.printf("  %-15s %4u  Core %d\n",
                      taskStatus.pcTaskName,
                      taskStatus.usStackHighWaterMark,
                      taskStatus.xCoreID);
      }

      Serial.println("==================");
      xSemaphoreGive(serialMutex);
    }

    // El Watchdog considera activa a la tarea despues de este incremento.
    heartbeats[STATISTICS]++;
    // El reporte no necesita ejecutarse continuamente.
    vTaskDelay(REPORT_PERIOD);
  }
}

// Supervisa que las demas tareas sigan actualizando su heartbeat.
void watchdogTask(void *parameter) {
  // Guarda el valor observado durante la comprobacion anterior.
  uint32_t previousHeartbeats[TASK_COUNT] = {};

  while (true) {
    for (uint8_t service = LOGGER; service < TASK_COUNT; service++) {
      // El Watchdog no puede supervisarse a si mismo.
      if (service == WATCHDOG) {
        continue;
      }

      // Si el contador no cambio, el servicio no tuvo actividad.
      if (heartbeats[service] == previousHeartbeats[service]) {
        char message[EVENT_TEXT_SIZE];
        snprintf(message, sizeof(message),
                 "Watchdog: sin actividad en %s", pcTaskGetName(taskHandles[service]));
        logEvent(message);
      }
      // Actualiza la referencia para la proxima ronda.
      previousHeartbeats[service] = heartbeats[service];
    }

    heartbeats[WATCHDOG]++;
    // Espera antes de volver a revisar todos los servicios.
    vTaskDelay(WATCHDOG_PERIOD);
  }
}

// Muestra los comandos disponibles en la consola.
void showHelp() {
  printLine("Comandos: help | tasks | status | pause sensor | resume sensor");
  printLine("          priority logger <0..configMAX_PRIORITIES-1>");
}

// Muestra prioridad y stack libre de cada servicio.
void showTasks() {
  if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
    for (uint8_t service = LOGGER; service < TASK_COUNT; service++) {
      TaskStatus_t taskStatus;
      // Consulta el estado actual sin detener la tarea.
      vTaskGetInfo(taskHandles[service], &taskStatus, pdTRUE, eInvalid);
      Serial.printf("%-15s prioridad %u, stack libre %u\n",
                    taskStatus.pcTaskName,
                    taskStatus.uxCurrentPriority,
                    taskStatus.usStackHighWaterMark);
    }
    xSemaphoreGive(serialMutex);
  }
}

// Muestra un resumen del estado del sensor y de la memoria.
void showStatus() {
  char status[EVENT_TEXT_SIZE];
  // eSuspended permite informar si el usuario pauso el Sensor.
  snprintf(status, sizeof(status), "Sensor=%s, valor=%d, heap=%u bytes",
           eTaskGetState(taskHandles[SENSOR]) == eSuspended ? "pausado" : "activo",
           sensorValue, ESP.getFreeHeap());
  printLine(status);
}

// Interpreta una linea recibida desde el monitor serie.
void processCommand(String command) {
  // Quita espacios y acepta comandos escritos en mayusculas.
  command.trim();
  command.toLowerCase();

  // Muestra la ayuda basica.
  if (command == "help") {
    showHelp();
  // Lista informacion de las tareas.
  } else if (command == "tasks") {
    showTasks();
  // Muestra el estado general del sistema.
  } else if (command == "status") {
    showStatus();
  // Suspende la tarea Sensor desde la consola.
  } else if (command == "pause sensor") {
    vTaskSuspend(taskHandles[SENSOR]);
    printLine("Sensor pausado.");
  // Reanuda la tarea Sensor suspendida.
  } else if (command == "resume sensor") {
    vTaskResume(taskHandles[SENSOR]);
    printLine("Sensor reanudado.");
  // Cambia la prioridad del Logger durante la ejecucion.
  } else if (command.startsWith("priority logger ")) {
    const int priority = command.substring(16).toInt();
    // Solo acepta prioridades validas para FreeRTOS.
    if (priority >= 0 && priority < configMAX_PRIORITIES) {
      vTaskPrioritySet(taskHandles[LOGGER], priority);
      printLine("Prioridad de Logger actualizada.");
    } else {
      printLine("Prioridad fuera de rango.");
    }
  // Informa cuando la linea no coincide con un comando conocido.
  } else if (command.length() > 0) {
    printLine("Comando desconocido. Escriba help.");
  }
}

// Lee el puerto serie y entrega cada linea al interprete de comandos.
void consoleTask(void *parameter) {
  String command;

  printLine("MiniSO listo. Escriba help para ver los comandos.");
  while (true) {
    // Puede recibir varios caracteres en una misma ejecucion.
    while (Serial.available() > 0) {
      const char character = static_cast<char>(Serial.read());
      if (character == '\n' || character == '\r') {
        // Enter indica que el comando esta completo.
        processCommand(command);
        command = "";
      } else {
        // Acumula los caracteres hasta encontrar Enter.
        command += character;
      }
    }

    // La consola tambien demuestra que sigue viva.
    heartbeats[CONSOLE]++;
    // Evita consultar Serial en un bucle ocupado.
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Inicializa hardware, recursos compartidos y tareas FreeRTOS.
void setup() {
  // Configura la velocidad del monitor serie.
  Serial.begin(115200);
  // Crea el mutex que protege las salidas por Serial.
  serialMutex = xSemaphoreCreateMutex();
  // Reserva espacio para ocho eventos pendientes.
  eventQueue = xQueueCreate(8, sizeof(Event));

  // Crea cada servicio con stack y prioridad iniciales.
  xTaskCreate(loggerTask, "Logger", 2048, nullptr, 2, &taskHandles[LOGGER]);
  xTaskCreate(sensorTask, "Sensor", 2048, nullptr, 1, &taskHandles[SENSOR]);
  xTaskCreate(communicationsTask, "Comms", 2048, nullptr, 1,
              &taskHandles[COMMUNICATIONS]);
  xTaskCreate(statisticsTask, "Stats", 3072, nullptr, 1, &taskHandles[STATISTICS]);
  xTaskCreate(watchdogTask, "Watchdog", 2048, nullptr, 3, &taskHandles[WATCHDOG]);
  xTaskCreate(consoleTask, "Console", 3072, nullptr, 2, &taskHandles[CONSOLE]);
}

// loop no realiza trabajo: todo corre dentro de tareas FreeRTOS.
void loop() {
  // Bloquea la tarea principal de Arduino para no consumir CPU.
  vTaskDelay(portMAX_DELAY);
}