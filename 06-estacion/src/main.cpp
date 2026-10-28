#include <Arduino.h>

/*
 * 06 - Estacion de monitoreo y control
 *
 * Este proyecto se construye por etapas. La version inicial ya permite
 * observar una medicion simulada y una decision de control. El resto de los
 * mecanismos de FreeRTOS se incorporan durante la clase.
 *
 * El objetivo no es esconder toda la logica en una funcion grande: cada
 * responsabilidad debe tener su tarea, cola, mutex o mecanismo de
 * sincronizacion justificado.
 */

constexpr TickType_t SENSOR_PERIOD = pdMS_TO_TICKS(2000);
constexpr TickType_t DIAGNOSTICS_PERIOD = pdMS_TO_TICKS(5000);
constexpr size_t COMMAND_SIZE = 64;

enum class EventType : uint8_t
{
    TemperatureHigh,
    WaterLow,
    SensorFailure,
    EmergencyStop
};

struct Measurement
{
    float temperature;
    float humidity;
    float waterLevel;
    uint32_t timestamp;
};

struct Command
{
    char text[COMMAND_SIZE];
};

struct SystemEvent
{
    EventType type;
    uint32_t timestamp;
};

struct SystemState
{
    Measurement lastMeasurement;
    bool fanOn;
    bool pumpOn;
    bool lightOn;
    bool alarmOn;
    bool automaticMode;
    uint32_t measurementsReceived;
    uint32_t eventsRaised;
    uint32_t commandsReceived;
    uint32_t commandsRejected;
};

QueueHandle_t measurementQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
QueueHandle_t eventQueue = nullptr;
SemaphoreHandle_t stateMutex = nullptr;

TaskHandle_t sensorTaskHandle = nullptr;
TaskHandle_t controlTaskHandle = nullptr;
TaskHandle_t commandTaskHandle = nullptr;
TaskHandle_t diagnosticsTaskHandle = nullptr;

SystemState state = {};

/*
 * ETAPA 1 - Sensores y tareas periodicas
 *
 * Esta tarea simula una medicion. En una version con hardware, este seria el
 * lugar para leer sensores reales, sin mezclar la lectura con las decisiones
 * de control ni con la interfaz de usuario.
 */
void sensorTask(void *parameter)
{
    (void)parameter;
    uint32_t sample = 0;

    for (;;)
    {
        Measurement measurement = {
            24.0f + static_cast<float>(sample % 12),
            55.0f - static_cast<float>(sample % 10),
            70.0f - static_cast<float>(sample % 16),
            millis()};

        /*
         * ETAPA 2 - Productor-consumidor
         *
         * Publicar una copia de la medicion en measurementQueue. Decidir que
         * hacer si la cola esta llena y registrar el descarte.
         */
        if (xQueueSend(measurementQueue, &measurement, 0) != pdTRUE)
        {
            Serial.println("[Sensor] Se descarto una medicion: cola llena.");
        }

        sample++;
        vTaskDelay(SENSOR_PERIOD);
    }
}

/*
 * ETAPA 2 - Tarea de control
 *
 * Consume mediciones y aplica reglas sobre los actuadores. La version base
 * solo actualiza el estado y muestra una decision sencilla. Las reglas, los
 * eventos y las alarmas se completan progresivamente.
 */
void controlTask(void *parameter)
{
    (void)parameter;
    Measurement measurement = {};

    for (;;)
    {
        if (xQueueReceive(measurementQueue, &measurement, portMAX_DELAY) ==
            pdTRUE)
        {
            bool fanOn = false;
            bool pumpOn = false;

            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE)
            {
                state.lastMeasurement = measurement;
                state.measurementsReceived++;

                /*
                 * ETAPA 3 - Control automatico
                 *
                 * Completar reglas para ventilador, bomba, luz y alarma.
                 * No modificar el estado desde otras tareas sin protegerlo.
                 */
                if (state.automaticMode)
                {
                    state.fanOn = measurement.temperature >= 30.0f;
                    state.pumpOn = measurement.waterLevel <= 25.0f;
                }

                fanOn = state.fanOn;
                pumpOn = state.pumpOn;
                xSemaphoreGive(stateMutex);
            }

            Serial.printf("[Control] T=%.1f C, H=%.1f %%, agua=%.1f %%, "
                          "ventilador=%s, bomba=%s\n",
                          measurement.temperature,
                          measurement.humidity,
                          measurement.waterLevel,
                          fanOn ? "ON" : "OFF",
                          pumpOn ? "ON" : "OFF");
        }
    }
}

/*
 * ETAPA 4 - Consola y comandos
 *
 * La consola es otra interfaz del sistema. No deberia modificar directamente
 * los actuadores: debe interpretar el texto y publicar un Command en
 * commandQueue para que otra parte del sistema lo procese.
 */
void commandTask(void *parameter)
{
    (void)parameter;
    Command command = {};

    for (;;)
    {
        if (Serial.available() > 0)
        {
            const size_t length = Serial.readBytesUntil(
                '\n', command.text, COMMAND_SIZE - 1);
            command.text[length] = '\0';

            // TODO (clase): validar el comando y enviarlo a commandQueue.
            Serial.print("[Consola] Recibido: ");
            Serial.println(command.text);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/*
 * ETAPA 5 - Diagnostico
 *
 * Leer el estado bajo el mutex y observar recursos del sistema. Luego agregar
 * cantidad de elementos en las colas, high water mark y heap disponible.
 */
void diagnosticsTask(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        vTaskDelay(DIAGNOSTICS_PERIOD);

        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            const SystemState current = state;
            xSemaphoreGive(stateMutex);

            Serial.printf(
                "[Diag] mediciones=%lu, comandos=%lu, rechazados=%lu, "
                "heap=%lu, stackControl=%lu\n",
                static_cast<unsigned long>(current.measurementsReceived),
                static_cast<unsigned long>(current.commandsReceived),
                static_cast<unsigned long>(current.commandsRejected),
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(
                    uxTaskGetStackHighWaterMark(controlTaskHandle)));
        }
        else
        {
            Serial.println("[Diag] No se pudo obtener el mutex del estado.");
        }
    }
}

/*
 * ETAPA 6 - Eventos y alarmas
 *
 * Crear alarmTask para consumir eventQueue. Los eventos pueden originarse en
 * controlTask o en una tarea dedicada a validar sensores.
 */
void alarmTask(void *parameter)
{
    (void)parameter;

    // TODO (clase): recibir SystemEvent y activar/desactivar la alarma.
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
 * ETAPA 7 - Temporizadores, notificaciones y grupos de eventos
 *
 * Reservar estos mecanismos para cuando la arquitectura base funcione:
 * - timer: apagar la bomba automaticamente luego de un tiempo;
 * - notification: despertar alarmTask ante una emergencia;
 * - event group: SYSTEM_READY, NETWORK_READY y EMERGENCY_STOP;
 * - semaphore: representar una interrupcion o disponibilidad de hardware.
 */

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n06 - Estacion de monitoreo y control");
    Serial.println("Escriba help cuando la consola este implementada.");

    measurementQueue = xQueueCreate(5, sizeof(Measurement));
    commandQueue = xQueueCreate(5, sizeof(Command));
    eventQueue = xQueueCreate(8, sizeof(SystemEvent));
    stateMutex = xSemaphoreCreateMutex();

    if (measurementQueue == nullptr || commandQueue == nullptr ||
        eventQueue == nullptr || stateMutex == nullptr)
    {
        Serial.println("[Error] No se pudieron crear los recursos de RTOS.");
        return;
    }

    state.automaticMode = true;

    /*
     * ETAPA 1 - Crear solamente sensorTask y controlTask.
     * Las tareas restantes pueden habilitarse cuando se implemente cada etapa.
     */
    xTaskCreate(sensorTask, "Sensor", 3072, nullptr, 2, &sensorTaskHandle);
    xTaskCreate(controlTask, "Control", 4096, nullptr, 3, &controlTaskHandle);
    xTaskCreate(commandTask, "Command", 3072, nullptr, 1, &commandTaskHandle);
    xTaskCreate(diagnosticsTask, "Diagnostics", 3072, nullptr, 1,
                &diagnosticsTaskHandle);

    // TODO (clase): crear alarmTask, timers, semaforos y grupos de eventos.
}

void loop()
{
    /*
     * El trabajo pertenece a las tareas. Mantener loop vacio permite discutir
     * por que el planificador de FreeRTOS no necesita un segundo bucle propio.
     */
    vTaskDelay(pdMS_TO_TICKS(1000));
}
