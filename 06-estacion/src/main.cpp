#include <Arduino.h>
#include "freertos/event_groups.h"
#include "freertos/timers.h"

/*
 * 06 - Estacion de monitoreo y control
 *
 * La estacion simula sensores ambientales y actuadores sobre ESP32.
 * Cada responsabilidad se implementa en una tarea y los datos cruzan
 * contextos mediante las primitivas apropiadas de FreeRTOS.
 */

// Los periodos se expresan en ticks, no en milisegundos.
constexpr TickType_t SENSOR_PERIOD = pdMS_TO_TICKS(2000);
constexpr TickType_t DIAGNOSTICS_PERIOD = pdMS_TO_TICKS(5000);
constexpr TickType_t COMMAND_POLL_PERIOD = pdMS_TO_TICKS(50);
constexpr TickType_t ACTUATOR_TIMEOUT = pdMS_TO_TICKS(50);
constexpr TickType_t PUMP_SAFETY_TIME = pdMS_TO_TICKS(10000);
// Limite fijo: no se reservan buffers dinamicos para cada linea recibida.
constexpr size_t COMMAND_TEXT_SIZE = 64;

// Cada bit representa una condicion global que puede permanecer activa.
constexpr EventBits_t SENSOR_READY_BIT = BIT0;
constexpr EventBits_t CONTROL_READY_BIT = BIT1;
constexpr EventBits_t EMERGENCY_STOP_BIT = BIT2;
// Ambas condiciones deben cumplirse para considerar la estacion inicializada.
constexpr EventBits_t SYSTEM_READY_BITS =
    SENSOR_READY_BIT | CONTROL_READY_BIT;

// Un evento describe una condicion anormal, no un dato del sensor.
enum class EventType : uint8_t
{
    TemperatureHigh,
    WaterLow,
    SensorFailure,
    EmergencyStop
};

// Contrato de datos que SensorTask entrega a ControlTask.
struct Measurement
{
    float temperature;
    float humidity;
    float waterLevel;
    uint32_t timestamp;
};

// El parser convierte texto en acciones antes de publicarlas en una cola.
enum class CommandType : uint8_t
{
    Help,
    Status,
    AutomaticOn,
    AutomaticOff,
    FanOn,
    FanOff,
    PumpOn,
    PumpOff,
    LightOn,
    LightOff,
    AlarmReset,
    EmergencyStop,
    Unknown
};

// Un comando se copia por valor; nunca se guarda un puntero al Serial.
struct Command
{
    CommandType type;
    char text[COMMAND_TEXT_SIZE];
};

// Contiene el tipo de evento y el momento aproximado en que ocurrio.
struct SystemEvent
{
    EventType type;
    uint32_t timestamp;
};

// Instantanea compartida del sistema. Se accede solo mediante stateMutex.
struct SystemState
{
    // Ultima medicion recibida por el controlador.
    Measurement lastMeasurement;
    // Estado visible de las tres salidas y la alarma.
    bool fanOn;
    bool pumpOn;
    bool lightOn;
    bool alarmOn;
    // En AUTO las reglas gobiernan; en MANUAL solo mandan los comandos.
    bool automaticMode;
    // Contadores utiles para medir carga y perdida de informacion.
    uint32_t measurementsReceived;
    uint32_t measurementsDropped;
    uint32_t eventsRaised;
    uint32_t eventsDropped;
    uint32_t commandsReceived;
    uint32_t commandsRejected;
};

// Transportan datos estructurados entre tareas.
QueueHandle_t measurementQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
QueueHandle_t eventQueue = nullptr;

// El mutex protege el estado; los otros mutex protegen recursos del sistema.
SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t actuatorSemaphore = nullptr;
// Representa la finalizacion de una conversion simulada.
SemaphoreHandle_t sensorConversionSemaphore = nullptr;

// Condiciones globales persistentes y temporizador de seguridad.
EventGroupHandle_t systemEvents = nullptr;
TimerHandle_t pumpSafetyTimer = nullptr;

// Los handles permiten referenciar tareas y reaccionar desde otras tareas.
TaskHandle_t sensorTaskHandle = nullptr;
TaskHandle_t controlTaskHandle = nullptr;
TaskHandle_t commandTaskHandle = nullptr;
TaskHandle_t diagnosticsTaskHandle = nullptr;
TaskHandle_t alarmTaskHandle = nullptr;

SystemState state = {};

// Traduce un evento a texto sin ejecutar acciones ni modificar el estado.
const char *eventName(EventType type)
{
    switch (type)
    {
    case EventType::TemperatureHigh:
        return "TEMPERATURE_HIGH";
    case EventType::WaterLow:
        return "WATER_LOW";
    case EventType::SensorFailure:
        return "SENSOR_FAILURE";
    case EventType::EmergencyStop:
        return "EMERGENCY_STOP";
    }

    return "UNKNOWN";
}

// Valida comparaciones completas: no acepta comandos parciales o ambiguos.
CommandType parseCommand(const char *text)
{
    if (strcmp(text, "help") == 0)
        return CommandType::Help;
    if (strcmp(text, "status") == 0)
        return CommandType::Status;
    if (strcmp(text, "auto on") == 0)
        return CommandType::AutomaticOn;
    if (strcmp(text, "auto off") == 0)
        return CommandType::AutomaticOff;
    if (strcmp(text, "fan on") == 0)
        return CommandType::FanOn;
    if (strcmp(text, "fan off") == 0)
        return CommandType::FanOff;
    if (strcmp(text, "pump on") == 0)
        return CommandType::PumpOn;
    if (strcmp(text, "pump off") == 0)
        return CommandType::PumpOff;
    if (strcmp(text, "light on") == 0)
        return CommandType::LightOn;
    if (strcmp(text, "light off") == 0)
        return CommandType::LightOff;
    if (strcmp(text, "alarm reset") == 0)
        return CommandType::AlarmReset;
    if (strcmp(text, "emergency stop") == 0)
        return CommandType::EmergencyStop;

    return CommandType::Unknown;
}

// Copia el estado completo bajo mutex y libera el recurso antes de imprimir.
bool copyState(SystemState &destination)
{
    if (xSemaphoreTake(stateMutex, ACTUATOR_TIMEOUT) != pdTRUE)
        return false;

    destination = state;
    xSemaphoreGive(stateMutex);
    return true;
}

// Lee una instantanea segura y imprime fuera de la seccion critica.
void printStatus()
{
    SystemState current = {};
    if (!copyState(current))
    {
        Serial.println("[Status] No se pudo leer el estado.");
        return;
    }

    Serial.printf("[Status] modo=%s, T=%.1f C, H=%.1f %%, agua=%.1f %%\n",
                  current.automaticMode ? "AUTO" : "MANUAL",
                  current.lastMeasurement.temperature,
                  current.lastMeasurement.humidity,
                  current.lastMeasurement.waterLevel);
    Serial.printf("[Status] fan=%s, pump=%s, light=%s, alarm=%s\n",
                  current.fanOn ? "ON" : "OFF",
                  current.pumpOn ? "ON" : "OFF",
                  current.lightOn ? "ON" : "OFF",
                  current.alarmOn ? "ON" : "OFF");
}

// Convierte una condicion en un evento transportado a AlarmTask.
void publishEvent(EventType type)
{
    const SystemEvent event = {type, millis()};

    // El envio sin bloqueo evita que el detector quede esperando recursos.
    if (xQueueSend(eventQueue, &event, 0) != pdTRUE)
    {
        if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            state.eventsDropped++;
            xSemaphoreGive(stateMutex);
        }
        Serial.printf("[Evento] Descartado: %s\n", eventName(type));
        return;
    }

    // El contador sirve para saber cuantos eventos llegaron a entrar.
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        state.eventsRaised++;
        xSemaphoreGive(stateMutex);
    }

    // La notificacion despierta a la tarea; la cola conserva el dato.
    if (alarmTaskHandle != nullptr)
        xTaskNotifyGive(alarmTaskHandle);
}

// Serializa el acceso y actualiza el estado solo despues de aplicar salidas.
bool setActuators(bool fan, bool pump, bool light)
{
    // Dos tareas no pueden modificar las salidas al mismo tiempo.
    if (xSemaphoreTake(actuatorSemaphore, ACTUATOR_TIMEOUT) != pdTRUE)
    {
        Serial.println("[Actuadores] Recurso ocupado.");
        return false;
    }

    Serial.printf("[Actuadores] ventilador=%s, bomba=%s, luz=%s\n",
                  fan ? "ON" : "OFF",
                  pump ? "ON" : "OFF",
                  light ? "ON" : "OFF");

    const bool stateUpdated =
        xSemaphoreTake(stateMutex, ACTUATOR_TIMEOUT) == pdTRUE;
    if (stateUpdated)
    {
        state.fanOn = fan;
        state.pumpOn = pump;
        state.lightOn = light;
        xSemaphoreGive(stateMutex);
    }
    xSemaphoreGive(actuatorSemaphore);
    if (!stateUpdated)
        Serial.println("[Actuadores] No se pudo actualizar el estado.");
    return stateUpdated;
}

// El callback queda corto: delega la accion real enviando un comando.
void pumpSafetyTimerCallback(TimerHandle_t timer)
{
    (void)timer;

    const Command command = {CommandType::PumpOff, "timer pump off"};
    if (xQueueSend(commandQueue, &command, 0) != pdTRUE)
        Serial.println("[Timer] No se pudo enviar apagado de seguridad.");
}

// Produce mediciones periodicas y simula el final de una conversion.
void sensorTask(void *parameter)
{
    (void)parameter;
    uint32_t sample = 0;
    bool firstMeasurement = true;

    for (;;)
    {
        // Cada medicion nace dentro de la tarea y se copia al enviarla.
        const Measurement measurement = {
            24.0f + static_cast<float>(sample % 12),
            55.0f - static_cast<float>(sample % 10),
            70.0f - static_cast<float>(sample % 16),
            millis()};

        // El semaforo binario representa una conversion disponible o completada.
        xSemaphoreGive(sensorConversionSemaphore);

        // Una cola llena significa que el consumidor no pudo seguir el ritmo.
        if (xQueueSend(measurementQueue, &measurement, 0) != pdTRUE)
        {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE)
            {
                state.measurementsDropped++;
                xSemaphoreGive(stateMutex);
            }
            Serial.println("[Sensor] Medicion descartada: cola llena.");
        }

        // El bit se mantiene activo hasta que otro componente lo limpie.
        if (firstMeasurement)
        {
            xEventGroupSetBits(systemEvents, SENSOR_READY_BIT);
            firstMeasurement = false;
        }

        sample++;
        vTaskDelay(SENSOR_PERIOD);
    }
}

void controlTask(void *parameter)
{
    (void)parameter;
    Measurement measurement = {};
    Command command = {};
    bool firstMeasurement = true;

    for (;;)
    {
        // Primero se agotan los comandos pendientes para reducir su latencia.
        while (xQueueReceive(commandQueue, &command, 0) == pdTRUE)
        {
            SystemState current = {};
            if (!copyState(current))
            {
                Serial.println("[Control] No se pudo leer el estado.");
                continue;
            }

            if (command.type == CommandType::Help)
            {
                // Help muestra la interfaz; no cambia ninguna salida.
                Serial.println("help | status | auto on/off | fan on/off");
                Serial.println("pump on/off | light on/off | alarm reset");
                Serial.println("emergency stop");
            }
            else if (command.type == CommandType::Status)
            {
                printStatus();
            }
            else if (command.type == CommandType::AutomaticOn ||
                     command.type == CommandType::AutomaticOff)
            {
                // Solo ControlTask cambia el modo operativo.
                if (xSemaphoreTake(stateMutex, ACTUATOR_TIMEOUT) == pdTRUE)
                {
                    state.automaticMode =
                        command.type == CommandType::AutomaticOn;
                    xSemaphoreGive(stateMutex);
                }
            }
            else if (command.type == CommandType::AlarmReset)
            {
                // El reset borra la emergencia persistente, no el historial.
                xEventGroupClearBits(systemEvents, EMERGENCY_STOP_BIT);
                if (xSemaphoreTake(stateMutex, ACTUATOR_TIMEOUT) == pdTRUE)
                {
                    state.alarmOn = false;
                    xSemaphoreGive(stateMutex);
                }
                Serial.println("[Control] Alarma reiniciada.");
            }
            else if (command.type == CommandType::EmergencyStop)
            {
                // Marca el estado seguro antes de publicar el evento.
                xEventGroupSetBits(systemEvents, EMERGENCY_STOP_BIT);
                publishEvent(EventType::EmergencyStop);
            }
            else if (!current.automaticMode || command.type == CommandType::PumpOff)
            {
                // Los comandos manuales conservan las otras salidas sin cambios.
                bool fan = current.fanOn;
                bool pump = current.pumpOn;
                bool light = current.lightOn;

                switch (command.type)
                {
                case CommandType::FanOn:
                    fan = true;
                    break;
                case CommandType::FanOff:
                    fan = false;
                    break;
                case CommandType::PumpOn:
                    pump = true;
                    break;
                case CommandType::PumpOff:
                    pump = false;
                    break;
                case CommandType::LightOn:
                    light = true;
                    break;
                case CommandType::LightOff:
                    light = false;
                    break;
                default:
                    break;
                }

                if (setActuators(fan, pump, light) && pump)
                    // Cada encendido rearma el limite de seguridad de la bomba.
                    xTimerStart(pumpSafetyTimer, 0);
                else if (!pump)
                    // Apagar la bomba cancela su proteccion temporal.
                    xTimerStop(pumpSafetyTimer, 0);
            }
            else
            {
                // En modo automatico, las ordenes manuales no deben prevalecer.
                Serial.println("[Control] Comando manual rechazado en AUTO.");
            }
        }

        // Un timeout corto mantiene viva esta tarea cuando no hay mediciones.
        if (xQueueReceive(measurementQueue, &measurement,
                          pdMS_TO_TICKS(100)) == pdTRUE)
        {
            SystemState current = {};
            if (xSemaphoreTake(stateMutex, ACTUATOR_TIMEOUT) != pdTRUE)
            {
                Serial.println("[Control] No se pudo actualizar el estado.");
                continue;
            }
            state.lastMeasurement = measurement;
            state.measurementsReceived++;
            current = state;
            xSemaphoreGive(stateMutex);

            // Histéresis: los umbrales de apagado son menores a los de encendido.
            if (current.automaticMode &&
                !(xEventGroupGetBits(systemEvents) & EMERGENCY_STOP_BIT))
            {
                const bool fan =
                    current.fanOn ? measurement.temperature >= 28.0f
                                  : measurement.temperature >= 30.0f;
                const bool pump =
                    current.pumpOn ? measurement.waterLevel <= 35.0f
                                   : measurement.waterLevel <= 25.0f;

                if (setActuators(fan, pump, current.lightOn) && pump)
                    xTimerStart(pumpSafetyTimer, 0);
                else if (!pump)
                    xTimerStop(pumpSafetyTimer, 0);
            }

            // Control detecta la condicion y publica el evento correspondiente.
            if (measurement.temperature >= 34.0f)
                publishEvent(EventType::TemperatureHigh);
            if (measurement.waterLevel <= 15.0f)
                publishEvent(EventType::WaterLow);

            // Solo la primera medicion activa el bit de control listo.
            if (firstMeasurement)
            {
                xEventGroupSetBits(systemEvents, CONTROL_READY_BIT);
                firstMeasurement = false;
            }

            Serial.printf("[Control] T=%.1f C, H=%.1f %%, agua=%.1f %%\n",
                          measurement.temperature,
                          measurement.humidity,
                          measurement.waterLevel);
        }
    }
}

// Lee el puerto serie, valida el texto y produce Command.
void commandTask(void *parameter)
{
    (void)parameter;
    Command command = {};

    for (;;)
    {
        // La consola no ocupa CPU si no hay datos disponibles.
        if (Serial.available() > 0)
        {
            // Se lee una linea completa y se reserva un byte para '\0'.
            const size_t length =
                Serial.readBytesUntil('\n', command.text,
                                      COMMAND_TEXT_SIZE - 1);
            command.text[length] = '\0';

            // El terminal puede enviar CRLF: se elimina el retorno de carro.
            if (length > 0 && command.text[length - 1] == '\r')
                command.text[length - 1] = '\0';

            // La validacion ocurre antes de publicar el comando.
            command.type = parseCommand(command.text);
            if (command.type == CommandType::Unknown)
            {
                if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE)
                {
                    state.commandsRejected++;
                    xSemaphoreGive(stateMutex);
                }
                Serial.println("[Consola] Comando invalido.");
            }
            // Un envio sin bloqueo evita congelar la interfaz serie.
            else if (xQueueSend(commandQueue, &command, 0) != pdTRUE)
            {
                Serial.println("[Consola] Cola de comandos llena.");
            }
            else if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) ==
                     pdTRUE)
            {
                state.commandsReceived++;
                xSemaphoreGive(stateMutex);
            }
        }

        // Se reduce la carga de la tarea mientras se espera nueva entrada.
        vTaskDelay(COMMAND_POLL_PERIOD);
    }
}

// Consume eventos y ejecuta las acciones asociadas a la alarma.
void alarmTask(void *parameter)
{
    (void)parameter;
    SystemEvent event = {};

    for (;;)
    {
        // Espera pasivamente hasta que un productor envie una notificacion.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Una notificacion puede representar varios eventos acumulados.
        while (xQueueReceive(eventQueue, &event, 0) == pdTRUE)
        {
            if (xSemaphoreTake(stateMutex, ACTUATOR_TIMEOUT) == pdTRUE)
            {
                state.alarmOn = true;
                xSemaphoreGive(stateMutex);
            }

            Serial.printf("[Alarma] Evento: %s\n", eventName(event.type));

            // Una emergencia siempre fuerza todas las salidas a apagado.
            if (event.type == EventType::EmergencyStop)
            {
                setActuators(false, false, false);
                xTimerStop(pumpSafetyTimer, 0);
                xEventGroupSetBits(systemEvents, EMERGENCY_STOP_BIT);
            }
        }
    }
}

// Observa el estado y los recursos sin retener el mutex durante Serial.
void diagnosticsTask(void *parameter)
{
    (void)parameter;

    for (;;)
    {
        // Este periodo es menos frecuente porque no participa del control.
        vTaskDelay(DIAGNOSTICS_PERIOD);

        SystemState current = {};
        if (copyState(current))
        {
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
            Serial.println("[Diag] No se pudo copiar el estado.");
        }
    }
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n06 - Estacion de monitoreo y control");

    // Las colas reciben copias de estructuras de tamano fijo.
    measurementQueue = xQueueCreate(5, sizeof(Measurement));
    commandQueue = xQueueCreate(8, sizeof(Command));
    eventQueue = xQueueCreate(8, sizeof(SystemEvent));

    // El primero protege datos; los siguientes protegen recursos de hardware.
    stateMutex = xSemaphoreCreateMutex();
    actuatorSemaphore = xSemaphoreCreateMutex();
    // El semaforo binario representa una unidad disponible al finalizar.
    sensorConversionSemaphore = xSemaphoreCreateBinary();
    // Condiciones globales que quedan visibles para varias tareas.
    systemEvents = xEventGroupCreate();
    // Timer one-shot: puede apagarse y rearmerse sin crear otra tarea.
    pumpSafetyTimer =
        xTimerCreate("PumpSafety", PUMP_SAFETY_TIME, pdFALSE, nullptr,
                     pumpSafetyTimerCallback);

    // Ninguna tarea deberia iniciarse con recursos RTOS incompletos.
    if (measurementQueue == nullptr || commandQueue == nullptr ||
        eventQueue == nullptr || stateMutex == nullptr ||
        actuatorSemaphore == nullptr || sensorConversionSemaphore == nullptr ||
        systemEvents == nullptr || pumpSafetyTimer == nullptr)
    {
        Serial.println("[Error] No se pudieron crear todos los recursos.");
        return;
    }

    // El arranque es automatico, con todas las salidas inicialmente apagadas.
    state.automaticMode = true;

    // Alarma: 4; control: 3; sensor: 2; consola y diagnostico: 1.
    xTaskCreate(alarmTask, "Alarm", 3072, nullptr, 4, &alarmTaskHandle);
    xTaskCreate(controlTask, "Control", 4096, nullptr, 3, &controlTaskHandle);
    xTaskCreate(sensorTask, "Sensor", 3072, nullptr, 2, &sensorTaskHandle);
    xTaskCreate(commandTask, "Command", 3072, nullptr, 1, &commandTaskHandle);
    xTaskCreate(diagnosticsTask, "Diagnostics", 3072, nullptr, 1,
                &diagnosticsTaskHandle);

    // Espera breve por las condiciones iniciales sin bloquear indefinidamente.
    xEventGroupWaitBits(systemEvents,
                        SYSTEM_READY_BITS,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(1000));

    Serial.println("Escriba help para ver los comandos.");
}

void loop()
{
    // setup crea las tareas: loop solo cede periodicamente el procesador.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
