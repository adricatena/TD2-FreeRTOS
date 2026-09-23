#include <Arduino.h>
#include "esp_timer.h"

// GPIO que genera la señal PWM.
constexpr uint8_t PWM_PIN = 16;
// Canal LEDC que usará el PWM.
constexpr uint8_t PWM_CHANNEL = 0;
// LED interno de la placa, conectado a la misma señal PWM.
constexpr uint8_t LED_PIN = 2;
// Canal LEDC independiente para el LED interno.
constexpr uint8_t LED_CHANNEL = 1;
// GPIO que recibe la señal para medirla.
constexpr uint8_t MEASURE_PIN = 17;
// Frecuencia configurada para el PWM.
constexpr uint32_t PWM_FREQUENCY = 10000;
// Resolución del duty cycle: valores de 0 a 255.
constexpr uint8_t PWM_RESOLUTION = 8;
// 128 de 255 representa aproximadamente un 50 %.
constexpr uint8_t PWM_DUTY = 128;

// Prioridad normal de la tarea medidora.
// ubastetype_t entero sin signo más eficiente según la arquitectura del procesador
constexpr UBaseType_t MEASURE_PRIORITY = 3;
// Prioridad normal de la tarea perturbadora.
constexpr UBaseType_t PERTURB_PRIORITY = 1; // en esp32 Equivale a uint32_t
// El control debe poder ejecutarse aunque la perturbadora no duerma.
constexpr UBaseType_t CONTROL_PRIORITY = 4;

// Handles necesarios para notificar tareas y cambiar sus prioridades.
// taskhandle: puntero al bloque de control de una tarea, Permite manipular esa tarea desde cualquier otra parte del código
TaskHandle_t measureTaskHandle = nullptr; //
TaskHandle_t perturbTaskHandle = nullptr;

// Variables compartidas entre la interrupción y la tarea medidora.
// volatile obliga a leer siempre el valor actualizado.
volatile int64_t risingEdgeTime = 0;
volatile int64_t pulsePeriod = 0;
volatile int64_t pulseWidth = 0;
volatile bool pulseStarted = false;

// ISR ejecutada en cada flanco de la señal medida.
// IRAM_ATTR obliga a ubicar una función en la memoria RAM en lugar de la memoria Flash
void IRAM_ATTR measureEdge()
{
  // Tiempo actual en microsegundos desde el reloj del sistema.
  const int64_t now = esp_timer_get_time();

  // Un nivel alto indica un flanco ascendente.
  if (digitalRead(MEASURE_PIN) == HIGH)
  {
    // Tiempo entre dos flancos ascendentes consecutivos.
    if (risingEdgeTime != 0)
    {
      pulsePeriod = now - risingEdgeTime;
    }

    // Guardamos el inicio del pulso actual.
    risingEdgeTime = now;
    pulseStarted = true;
  }
  // Un nivel bajo indica un flanco descendente.
  else if (pulseStarted)
  {
    // Tiempo transcurrido desde el flanco ascendente.
    pulseWidth = now - risingEdgeTime;

    // Despierta a la tarea medidora desde la ISR.
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(measureTaskHandle, &higherPriorityTaskWoken);

    // Solicita un cambio inmediato de tarea si corresponde.
    if (higherPriorityTaskWoken == pdTRUE)
    {
      portYIELD_FROM_ISR();
    }
  }
}

void pwmTask(void *parameter)
{
  /* 
  El ESP32 no usa un analogWrite() simple por software tradicional; tiene un periférico por hardware llamado LEDC (LED Control)
  diseñado para generar señales PWM precisas sin consumir CPU,
  ideal para brillo de LEDs, control de servos, velocidad de motores DC o zumbadores.
  El subsistema se compone de:
  Canales: En el ESP32 clásico hay 16 canales independientes (0 a 15).
  Frecuencia: Frecuencia de conmutación en Hertz (Hz).
  Resolución: Número de bits para el ciclo de trabajo (duty cycle), habitualmente de 1 a 16 bits. */

  // Configura el periférico LEDC, que genera PWM por hardware.
  ledcSetup(PWM_CHANNEL, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, PWM_DUTY);

  // El LED usa otro canal, pero exactamente la misma frecuencia y duty.
  ledcSetup(LED_CHANNEL, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcAttachPin(LED_PIN, LED_CHANNEL);
  ledcWrite(LED_CHANNEL, PWM_DUTY);

  // El hardware sigue generando PWM mientras la tarea espera.
  while (true)
  {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void measureTask(void *parameter)
{
  // Limita la salida serial a un mensaje por segundo.
  uint32_t lastReport = millis();

  while (true)
  {
    // Espera una medición nueva enviada por la ISR.
    // permite a una tarea bloquearse a la espera de una notificación directa
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Usa el período y el ancho capturados por la interrupción.
    if (millis() - lastReport >= 1000 && pulsePeriod > 0)
    {
      // Frecuencia = 1 / período.
      const float frequency = 1000000.0f / pulsePeriod;
      // Duty = tiempo en alto / período.
      const float duty = 100.0f * pulseWidth / pulsePeriod;
      const float generatedDuty = 100.0f * PWM_DUTY / ((1 << PWM_RESOLUTION) - 1);
      const float frequencyError = frequency - PWM_FREQUENCY;
      const float dutyError = duty - generatedDuty;

      // Compara en una línea la referencia configurada con lo capturado.
      Serial.printf("Generado: %lu Hz, duty = %.2f %% | Leido: %.2f Hz, ancho = %lld us, duty = %.2f %% | Error: frecuencia = %+.2f Hz, duty = %+.2f %%\n",
                    PWM_FREQUENCY, generatedDuty, frequency, pulseWidth, duty,
                    frequencyError, dutyError);
      lastReport = millis();
    }
  }
}

void perturbTask(void *parameter)
{
  // Variable para evitar que el compilador elimine el bucle pesado.
  volatile uint32_t value = 0;

  // Trabajo continuo: esta tarea nunca se bloquea voluntariamente.
  while (true)
  {
    // Carga artificial para ocupar el Core 1.
    value = value * 1664525UL + 1013904223UL;
  }
}

void controlTask(void *parameter)
{
  // Comienza con la medidora en prioridad alta.
  bool measurementHasPriority = true;

  while (true)
  {
    // Mantiene cada configuración durante cinco segundos.
    vTaskDelay(pdMS_TO_TICKS(5000));
    measurementHasPriority = !measurementHasPriority;

    // Alterna prioridades para observar el comportamiento del planificador.
    if (measurementHasPriority)
    {
      // La medidora puede desplazar a la tarea perturbadora.
      vTaskPrioritySet(measureTaskHandle, MEASURE_PRIORITY);
      vTaskPrioritySet(perturbTaskHandle, PERTURB_PRIORITY);
      Serial.printf("[PLANIFICADOR] Medicion prioritaria: Medidora P%u, Perturbadora P%u. La medicion tiene preferencia en Core 1.\n",
                    MEASURE_PRIORITY, PERTURB_PRIORITY);
    }
    else
    {
      // La perturbadora recibe temporalmente la prioridad más alta.
      vTaskPrioritySet(measureTaskHandle, PERTURB_PRIORITY);
      vTaskPrioritySet(perturbTaskHandle, MEASURE_PRIORITY);
      Serial.printf("[PLANIFICADOR] Carga prioritaria: Perturbadora P%u, Medidora P%u. La carga puede demorar los reportes de medicion.\n",
                    MEASURE_PRIORITY, PERTURB_PRIORITY);
    }
  }
}

void setup()
{
  // Inicializa el monitor serial.
  Serial.begin(115200);

  Serial.println();
  Serial.println("=== Profiling de PWM con FreeRTOS ===");
  Serial.printf("Generador: GPIO %u | Medicion: GPIO %u (conectar ambos)\n",
                PWM_PIN, MEASURE_PIN);
  Serial.printf("PWM esperado: %lu Hz | duty: %.2f %% | LED interno: GPIO %u\n",
                PWM_FREQUENCY,
                100.0f * PWM_DUTY / ((1 << PWM_RESOLUTION) - 1),
                LED_PIN);
  Serial.println("El LED interno reproduce el PWM por hardware; su brillo representa el duty.");

  // Configura el GPIO de entrada y la interrupción por ambos flancos.
  pinMode(MEASURE_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(MEASURE_PIN), measureEdge, CHANGE);

  // xTaskCreatePinnedToCore extensión de xTaskCreate para multicore. Permite fijar una tarea a un núcleo específico de la CPU
  // PWM por hardware en Core 0.
  xTaskCreatePinnedToCore(pwmTask, "PWM", 2048, nullptr, 2, nullptr, 0);

  // Medición y perturbación comparten el Core 1.
  xTaskCreatePinnedToCore(measureTask, "Medidora", 2048, nullptr,
                          MEASURE_PRIORITY, &measureTaskHandle, 1);
  xTaskCreatePinnedToCore(perturbTask, "Perturbadora", 2048, nullptr,
                          PERTURB_PRIORITY, &perturbTaskHandle, 1);

  // Controla el cambio de prioridades en caliente.
  xTaskCreatePinnedToCore(controlTask, "Control", 2048, nullptr,
                          CONTROL_PRIORITY, nullptr, 1);
}

void loop()
{
  // El trabajo principal está implementado en tareas FreeRTOS.
  vTaskDelay(portMAX_DELAY);
}