#include <Arduino.h>

// LED integrado y períodos de ejecución de las tareas.
constexpr uint8_t LED_PIN = 2; // constexpr es Reemplazo seguro de #define, el compilador incrusta directamente el resultado en la memoria flash
// pdMS_TO_TICKS convierte milisegundos a ticks del sistema
constexpr TickType_t LED_HALF_PERIOD = pdMS_TO_TICKS(250); // tiempo en unidades de ticks del sistema
constexpr TickType_t OPERATIONS_PERIOD = pdMS_TO_TICKS(5000);
bool ledState = false;

// Cambia el estado del LED cada 250 ms.
void blinkTask(void *parameter) {
  // devuelve la cantidad de ticks transcurridos desde que arrancó el planificador (scheduler)
  TickType_t lastWakeTime = xTaskGetTickCount();

  while (true) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
    // función diseñada para ejecutar tareas con una frecuencia periódica constante y exacta
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

    // pone a la tarea que la invoca en estado Bloqueado (Blocked) durante una cantidad específica de ticks del sistema
    vTaskDelay(OPERATIONS_PERIOD);
  }
}

void setup() {
  // Configura el hardware y la comunicación serie.
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, ledState); // inciamos el pin apagado

  // Inicia las dos tareas independientes de FreeRTOS. cuanto mas alta la prioridad, "mas importante" es la tarea.
  xTaskCreate(blinkTask, "Blink", 2048, nullptr, 2, nullptr);
  xTaskCreate(operationsTask, "Operations", 2048, nullptr, 1, nullptr);
  /* FreeRTOS ejecutará primero el blink cuando ambas tareas estén listas */
}

// El trabajo se ejecuta dentro de las tareas FreeRTOS.
void loop() {
}