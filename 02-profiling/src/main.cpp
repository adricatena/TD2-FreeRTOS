#include <Arduino.h>
#include "esp_timer.h"

// GPIO que genera la señal PWM.
constexpr uint8_t PWM_PIN = 16;
// GPIO que recibe la señal para medirla.
constexpr uint8_t MEASURE_PIN = 17;
// Frecuencia configurada para el PWM.
constexpr uint32_t PWM_FREQUENCY = 10000;
// Resolución del duty cycle: valores de 0 a 255.
constexpr uint8_t PWM_RESOLUTION = 8;
// 128 de 255 representa aproximadamente un 50 %.
constexpr uint8_t PWM_DUTY = 128;

// Prioridad normal de la tarea medidora.
constexpr UBaseType_t MEASURE_PRIORITY = 3;
// Prioridad normal de la tarea perturbadora.
constexpr UBaseType_t PERTURB_PRIORITY = 1;
// El control debe poder ejecutarse aunque la perturbadora no duerma.
constexpr UBaseType_t CONTROL_PRIORITY = 4;

// Handles necesarios para notificar tareas y cambiar sus prioridades.
TaskHandle_t measureTaskHandle = nullptr;
TaskHandle_t perturbTaskHandle = nullptr;

// Variables compartidas entre la interrupción y la tarea medidora.
// volatile obliga a leer siempre el valor actualizado.
volatile int64_t risingEdgeTime = 0;
volatile int64_t pulsePeriod = 0;
volatile int64_t pulseWidth = 0;
volatile bool pulseStarted = false;

// ISR ejecutada en cada flanco de la señal medida.
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
  // Configura el periférico LEDC, que genera PWM por hardware.
  ledcAttach(PWM_PIN, PWM_FREQUENCY, PWM_RESOLUTION);
  ledcWrite(PWM_PIN, PWM_DUTY);

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
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Usa el período y el ancho capturados por la interrupción.
    if (millis() - lastReport >= 1000 && pulsePeriod > 0)
    {
      // Frecuencia = 1 / período.
      const float frequency = 1000000.0f / pulsePeriod;
      // Duty = tiempo en alto / período.
      const float duty = 100.0f * pulseWidth / pulsePeriod;

      // Compara indirectamente el PWM generado con el PWM medido.
      Serial.printf("Medido: frecuencia = %.2f Hz, ancho = %lld us, duty = %.2f %%\n",
                    frequency, pulseWidth, duty);
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
      Serial.println("Prioridades: medidora alta, perturbadora baja");
    }
    else
    {
      // La perturbadora recibe temporalmente la prioridad más alta.
      vTaskPrioritySet(measureTaskHandle, PERTURB_PRIORITY);
      vTaskPrioritySet(perturbTaskHandle, MEASURE_PRIORITY);
      Serial.println("Prioridades: perturbadora alta, medidora baja");
    }
  }
}

void setup()
{
  // Inicializa el monitor serial.
  Serial.begin(115200);

  // Configura el GPIO de entrada y la interrupción por ambos flancos.
  pinMode(MEASURE_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(MEASURE_PIN), measureEdge, CHANGE);

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