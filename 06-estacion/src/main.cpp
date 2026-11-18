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

constexpr TickType_t SENSOR_PERIOD = pdMS_TO_TICKS(2000);
constexpr TickType_t DIAGNOSTICS_PERIOD = pdMS_TO_TICKS(5000);
constexpr TickType_t COMMAND_POLL_PERIOD = pdMS_TO_TICKS(50);
constexpr TickType_t ACTUATOR_TIMEOUT = pdMS_TO_TICKS(50);
constexpr TickType_t PUMP_SAFETY_TIME = pdMS_TO_TICKS(10000);
constexpr size_t COMMAND_TEXT_SIZE = 64;

constexpr EventBits_t SENSOR_READY_BIT = BIT0;
constexpr EventBits_t CONTROL_READY_BIT = BIT1;
constexpr EventBits_t EMERGENCY_STOP_BIT = BIT2;
constexpr EventBits_t SYSTEM_READY_BITS =
    SENSOR_READY_BIT | CONTROL_READY_BIT;

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

struct Command
{
    CommandType type;
    char text[COMMAND_TEXT_SIZE];
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
    uint32_t measurementsDropped;
    uint32_t eventsRaised;
    uint32_t eventsDropped;
    uint32_t commandsReceived;
    uint32_t commandsRejected;
};

QueueHandle_t measurementQueue = nullptr;
QueueHandle_t commandQueue = nullptr;
QueueHandle_t eventQueue = nullptr;

SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t actuatorSemaphore = nullptr;
SemaphoreHandle_t sensorConversionSemaphore = nullptr;

EventGroupHandle_t systemEvents = nullptr;
TimerHandle_t pumpSafetyTimer = nullptr;

TaskHandle_t sensorTaskHandle = nullptr;
TaskHandle_t controlTaskHandle = nullptr;
TaskHandle_t commandTaskHandle = nullptr;
TaskHandle_t diagnosticsTaskHandle = nullptr;
TaskHandle_t alarmTaskHandle = nullptr;

SystemState state = {};

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

bool copyState(SystemState &destination)
{
    if (xSemaphoreTake(stateMutex, ACTUATOR_TIMEOUT) != pdTRUE)
        return false;

    destination = state;
    xSemaphoreGive(stateMutex);
    return true;
}

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

void publishEvent(EventType type)
{
    const SystemEvent event = {type, millis()};

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

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        state.eventsRaised++;
        xSemaphoreGive(stateMutex);
    }

    if (alarmTaskHandle != nullptr)
        xTaskNotifyGive(alarmTaskHandle);
}

bool setActuators(bool fan, bool pump, bool light)
{
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

void pumpSafetyTimerCallback(TimerHandle_t timer)
{
    (void)timer;

    const Command command = {CommandType::PumpOff, "timer pump off"};
    if (xQueueSend(commandQueue, &command, 0) != pdTRUE)
        Serial.println("[Timer] No se pudo enviar apagado de seguridad.");
}

void sensorTask(void *parameter)
{
    (void)parameter;
    uint32_t sample = 0;
    bool firstMeasurement = true;

    for (;;)
    {
        const Measurement measurement = {
            24.0f + static_cast<float>(sample % 12),
            55.0f - static_cast<float>(sample % 10),
            70.0f - static_cast<float>(sample % 16),
            millis()};

        xSemaphoreGive(sensorConversionSemaphore);

        if (xQueueSend(measurementQueue, &measurement, 0) != pdTRUE)
        {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE)
            {
                state.measurementsDropped++;
                xSemaphoreGive(stateMutex);
            }
            Serial.println("[Sensor] Medicion descartada: cola llena.");
        }

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
                if (xSemaphoreTake(stateMutex, ACTUATOR_TIMEOUT) == pdTRUE)
                {
                    state.automaticMode =
                        command.type == CommandType::AutomaticOn;
                    xSemaphoreGive(stateMutex);
                }
            }
            else if (command.type == CommandType::AlarmReset)
            {
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
                xEventGroupSetBits(systemEvents, EMERGENCY_STOP_BIT);
                publishEvent(EventType::EmergencyStop);
            }
            else if (!current.automaticMode || command.type == CommandType::PumpOff)
            {
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
                    xTimerStart(pumpSafetyTimer, 0);
                else if (!pump)
                    xTimerStop(pumpSafetyTimer, 0);
            }
            else
            {
                Serial.println("[Control] Comando manual rechazado en AUTO.");
            }
        }

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

            if (measurement.temperature >= 34.0f)
                publishEvent(EventType::TemperatureHigh);
            if (measurement.waterLevel <= 15.0f)
                publishEvent(EventType::WaterLow);

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

void commandTask(void *parameter)
{
    (void)parameter;
    Command command = {};

    for (;;)
    {
        if (Serial.available() > 0)
        {
            const size_t length =
                Serial.readBytesUntil('\n', command.text,
                                      COMMAND_TEXT_SIZE - 1);
            command.text[length] = '\0';

            if (length > 0 && command.text[length - 1] == '\r')
                command.text[length - 1] = '\0';

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

        vTaskDelay(COMMAND_POLL_PERIOD);
    }
}

void alarmTask(void *parameter)
{
    (void)parameter;
    SystemEvent event = {};

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (xQueueReceive(eventQueue, &event, 0) == pdTRUE)
        {
            if (xSemaphoreTake(stateMutex, ACTUATOR_TIMEOUT) == pdTRUE)
            {
                state.alarmOn = true;
                xSemaphoreGive(stateMutex);
            }

            Serial.printf("[Alarma] Evento: %s\n", eventName(event.type));

            if (event.type == EventType::EmergencyStop)
            {
                setActuators(false, false, false);
                xTimerStop(pumpSafetyTimer, 0);
                xEventGroupSetBits(systemEvents, EMERGENCY_STOP_BIT);
            }
        }
    }
}

void diagnosticsTask(void *parameter)
{
    (void)parameter;

    for (;;)
    {
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

    measurementQueue = xQueueCreate(5, sizeof(Measurement));
    commandQueue = xQueueCreate(8, sizeof(Command));
    eventQueue = xQueueCreate(8, sizeof(SystemEvent));

    stateMutex = xSemaphoreCreateMutex();
    actuatorSemaphore = xSemaphoreCreateMutex();
    sensorConversionSemaphore = xSemaphoreCreateBinary();
    systemEvents = xEventGroupCreate();
    pumpSafetyTimer =
        xTimerCreate("PumpSafety", PUMP_SAFETY_TIME, pdFALSE, nullptr,
                     pumpSafetyTimerCallback);

    if (measurementQueue == nullptr || commandQueue == nullptr ||
        eventQueue == nullptr || stateMutex == nullptr ||
        actuatorSemaphore == nullptr || sensorConversionSemaphore == nullptr ||
        systemEvents == nullptr || pumpSafetyTimer == nullptr)
    {
        Serial.println("[Error] No se pudieron crear todos los recursos.");
        return;
    }

    state.automaticMode = true;

    xTaskCreate(alarmTask, "Alarm", 3072, nullptr, 4, &alarmTaskHandle);
    xTaskCreate(controlTask, "Control", 4096, nullptr, 3, &controlTaskHandle);
    xTaskCreate(sensorTask, "Sensor", 3072, nullptr, 2, &sensorTaskHandle);
    xTaskCreate(commandTask, "Command", 3072, nullptr, 1, &commandTaskHandle);
    xTaskCreate(diagnosticsTask, "Diagnostics", 3072, nullptr, 1,
                &diagnosticsTaskHandle);

    xEventGroupWaitBits(systemEvents,
                        SYSTEM_READY_BITS,
                        pdFALSE,
                        pdTRUE,
                        pdMS_TO_TICKS(1000));

    Serial.println("Escriba help para ver los comandos.");
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
