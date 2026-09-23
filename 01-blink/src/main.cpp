#include <Arduino.h>

// LED integrado y períodos de ejecución de las tareas.
constexpr uint8_t LED_PIN = 2;
constexpr TickType_t LED_HALF_PERIOD = pdMS_TO_TICKS(250);
constexpr TickType_t OPERATIONS_PERIOD = pdMS_TO_TICKS(5000);

// Cambia el estado del LED cada 250 ms.
void blinkTask(void *parameter) {
  TickType_t lastWakeTime = xTaskGetTickCount();

  while (true) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    vTaskDelayUntil(&lastWakeTime, LED_HALF_PERIOD);
  }
}

// Genera números y muestra sus operaciones por el monitor serie.
void operationsTask(void *parameter) {
  while (true) {
    // random() usa un límite superior exclusivo: [1, 101) equivale a 1..100.
    int firstNumber = random(1, 101);
    int secondNumber = random(1, 101);

    Serial.printf("\nNumeros: %d y %d\n", firstNumber, secondNumber);
    Serial.printf("Suma: %d\n", firstNumber + secondNumber);
    Serial.printf("Resta: %d\n", firstNumber - secondNumber);
    Serial.printf("Multiplicacion: %d\n", firstNumber * secondNumber);
    Serial.printf("Division: %.2f\n", (float) firstNumber / secondNumber);

    vTaskDelay(OPERATIONS_PERIOD);
  }
}

void setup() {
  // Configura el hardware y la comunicación serie.
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);

  // Inicia las dos tareas independientes de FreeRTOS.
  xTaskCreate(blinkTask, "Blink", 2048, nullptr, 1, nullptr);
  xTaskCreate(operationsTask, "Operations", 2048, nullptr, 1, nullptr);
}

// El trabajo se ejecuta dentro de las tareas FreeRTOS.
void loop() {
}