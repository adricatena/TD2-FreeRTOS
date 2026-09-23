#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// Proporciona funciones generales del sistema ESP32.
#include "esp_system.h"

// Nombre visible de la red creada por el ESP32.
constexpr char ACCESS_POINT_NAME[] = "ESP32-RTOS";
// Clave minima requerida por el modo punto de acceso.
constexpr char ACCESS_POINT_PASSWORD[] = "freertos";
// Intervalo entre lecturas del sensor Hall.
constexpr TickType_t SENSOR_PERIOD = pdMS_TO_TICKS(250);
// Intervalo de actualizacion de las metricas.
constexpr TickType_t DIAGNOSTICS_PERIOD = pdMS_TO_TICKS(1000);

// Datos que el servidor muestra en el panel web.
struct SystemSnapshot
{
    // Ultima lectura obtenida del sensor Hall interno.
    int hallValue;
    // Memoria dinamica disponible en bytes.
    uint32_t freeHeap;
    // Minima pila libre de la tarea Sensor.
    uint32_t sensorStack;
    // Minima pila libre de la tarea Diagnostico.
    uint32_t diagnosticsStack;
    // Porcentaje estimado de actividad del Core 0.
    float cpu0Usage;
    // Porcentaje estimado de actividad del Core 1.
    float cpu1Usage;
    // Segundos transcurridos desde el arranque.
    uint32_t uptimeSeconds;
};

// Servidor HTTP que atiende las solicitudes del celular.
AsyncWebServer server(80);
// Protege la instantanea mientras una tarea la actualiza.
SemaphoreHandle_t snapshotMutex = nullptr;
// Handle de la tarea que lee el sensor.
TaskHandle_t sensorTaskHandle = nullptr;
// Handle de la tarea que actualiza las metricas.
TaskHandle_t diagnosticsTaskHandle = nullptr;
// Ultimos valores publicados por las tareas del sistema.
SystemSnapshot snapshot = {};

// Lee el sensor interno en el nucleo reservado para el trabajo determinista.
void sensorTask(void *parameter)
{
    while (true)
    {
        // hallRead devuelve la medicion actual del sensor Hall interno.
        const int currentHallValue = hallRead();

        // Toma el mutex solo durante la copia de los datos.
        if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            // Publica la medicion para que la use el servidor web.
            snapshot.hallValue = currentHallValue;
            // Consulta el minimo historico de pila libre de esta tarea.
            snapshot.sensorStack = uxTaskGetStackHighWaterMark(nullptr);
            // Permite que otra tarea consulte la instantanea.
            xSemaphoreGive(snapshotMutex);
        }

        // Libera el Core 1 hasta la proxima lectura.
        vTaskDelay(SENSOR_PERIOD);
    }
}

// Cada monitor corre en su nucleo y compara su capacidad de trabajo con una referencia.
void cpuMonitorTask(void *parameter)
{
    // El parametro indica si esta tarea pertenece al Core 0 o al Core 1.
    const uint8_t core = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(parameter));
    // Se establece durante la primera medicion de la tarea.
    uint32_t referenceIterations = 0;
    // volatile evita que el compilador elimine el trabajo de prueba.
    volatile uint32_t iterations = 0;

    while (true)
    {
        // Reinicia el contador para la nueva ventana de medicion.
        iterations = 0;
        // La ventana de prueba dura 10 ms.
        const int64_t sampleStart = esp_timer_get_time();
        // Cuenta cuanto trabajo puede ejecutar este nucleo en esa ventana.
        while (esp_timer_get_time() - sampleStart < 10000)
        {
            iterations++;
        }

        // La primera muestra representa el 100 % de capacidad disponible.
        if (referenceIterations == 0)
        {
            referenceIterations = iterations;
        }

        // Menos iteraciones indican que otras tareas ocuparon el nucleo.
        const float cpuUsage = constrain(
            100.0f - (100.0f * iterations / referenceIterations), 0.0f, 100.0f);

        // Publica la medicion correspondiente al nucleo de esta tarea.
        if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            if (core == 0)
            {
                // Guarda la estimacion del Core 0.
                snapshot.cpu0Usage = cpuUsage;
            }
            else
            {
                // Guarda la estimacion del Core 1.
                snapshot.cpu1Usage = cpuUsage;
            }
            // Libera la instantanea para las demas tareas.
            xSemaphoreGive(snapshotMutex);
        }

        // Espera el resto del periodo antes de medir otra vez.
        vTaskDelay(DIAGNOSTICS_PERIOD - pdMS_TO_TICKS(10));
    }
}

// Actualiza las metricas generales del sistema desde el Core 1.
void diagnosticsTask(void *parameter)
{
    while (true)
    {
        // No hace falta calcular las metricas continuamente.
        vTaskDelay(DIAGNOSTICS_PERIOD);

        // Copia los datos generales en una unica seccion protegida.
        if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            // xPortGetFreeHeapSize informa el heap disponible para el programa.
            snapshot.freeHeap = xPortGetFreeHeapSize();
            // Consulta el minimo historico de pila libre de esta tarea.
            snapshot.diagnosticsStack = uxTaskGetStackHighWaterMark(nullptr);
            // millis permite mostrar cuanto tiempo lleva activo el sistema.
            snapshot.uptimeSeconds = millis() / 1000UL;
            // Libera el mutex despues de actualizar todos los campos.
            xSemaphoreGive(snapshotMutex);
        }
    }
}

// Arma el JSON sin mantener el mutex durante la respuesta de red.
String snapshotAsJson()
{
    // La copia evita acceder al estado compartido mientras se arma el texto.
    SystemSnapshot currentSnapshot = {};

    // Solo se bloquea el estado durante una asignacion corta.
    if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        currentSnapshot = snapshot;
        xSemaphoreGive(snapshotMutex);
    }

    // Comienza el objeto JSON que consumira el codigo JavaScript.
    String json = "{";
    json += "\"hall\":" + String(currentSnapshot.hallValue);
    json += ",\"heap\":" + String(currentSnapshot.freeHeap);
    json += ",\"sensorStack\":" + String(currentSnapshot.sensorStack);
    json += ",\"diagnosticsStack\":" + String(currentSnapshot.diagnosticsStack);
    json += ",\"cpu0\":" + String(currentSnapshot.cpu0Usage, 1);
    json += ",\"cpu1\":" + String(currentSnapshot.cpu1Usage, 1);
    json += ",\"uptime\":" + String(currentSnapshot.uptimeSeconds);
    // Cierra el objeto JSON antes de devolverlo al cliente.
    json += "}";
    return json;
}

// Pagina completa almacenada en memoria de programa para ahorrar RAM.
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Diagnostico FreeRTOS</title>
  <style>
    body { font-family: sans-serif; max-width: 560px; margin: 2rem auto; padding: 0 1rem; }
    h1 { font-size: 1.5rem; }
    section { border: 1px solid #bbb; padding: 1rem; margin: 1rem 0; }
    p { display: flex; justify-content: space-between; margin: .7rem 0; }
    strong { font-variant-numeric: tabular-nums; }
  </style>
</head>
<body>
  <h1>Panel de diagnostico FreeRTOS</h1>
  <section>
    <p><span>CPU Core 0</span><strong id="cpu0">-</strong></p>
    <p><span>CPU Core 1</span><strong id="cpu1">-</strong></p>
    <p><span>Heap libre</span><strong id="heap">-</strong></p>
    <p><span>Sensor Hall</span><strong id="hall">-</strong></p>
    <p><span>Tiempo activo</span><strong id="uptime">-</strong></p>
  </section>
  <section>
    <h2>Pila minima libre (palabras)</h2>
    <p><span>Tarea Sensor, Core 1</span><strong id="sensorStack">-</strong></p>
    <p><span>Tarea Diagnostico, Core 1</span><strong id="diagnosticsStack">-</strong></p>
  </section>
  <script>
    // Solicita una nueva instantanea al servidor.
    async function updatePanel() {
      const response = await fetch('/api/status');
      const data = await response.json();
      // Actualiza cada indicador con el valor recibido.
      document.querySelector('#cpu0').textContent = data.cpu0 + ' %';
      document.querySelector('#cpu1').textContent = data.cpu1 + ' %';
      document.querySelector('#heap').textContent = data.heap + ' bytes';
      document.querySelector('#hall').textContent = data.hall;
      document.querySelector('#uptime').textContent = data.uptime + ' s';
      document.querySelector('#sensorStack').textContent = data.sensorStack;
      document.querySelector('#diagnosticsStack').textContent = data.diagnosticsStack;
    }
    // Carga datos apenas termina de abrirse la pagina.
    updatePanel();
    // Mantiene el panel actualizado una vez por segundo.
    setInterval(updatePanel, 1000);
  </script>
</body>
</html>
)rawliteral";

void setup()
{
    // Inicia el puerto serie para observar mensajes de arranque.
    Serial.begin(115200);
    // Crea el mutex que protege la instantanea compartida.
    snapshotMutex = xSemaphoreCreateMutex();

    // Configura el ESP32 como punto de acceso, no como cliente.
    WiFi.mode(WIFI_AP);
    // Inicia la red Wi-Fi que usara el celular.
    WiFi.softAP(ACCESS_POINT_NAME, ACCESS_POINT_PASSWORD);

    // Ruta principal: devuelve la interfaz HTML del panel.
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(200, "text/html; charset=utf-8", INDEX_HTML); });
    // Ruta de datos: devuelve una instantanea en formato JSON.
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(200, "application/json", snapshotAsJson()); });
    // Comienza a aceptar solicitudes sin bloquear las tareas de usuario.
    server.begin();

    // Informa por Serial la direccion que debe abrir el alumno.
    Serial.println("Punto de acceso listo.");
    Serial.print("Abrir http://");
    Serial.println(WiFi.softAPIP());

    // Sensor y diagnostico comparten el Core 1.
    xTaskCreatePinnedToCore(sensorTask, "Sensor", 2048, nullptr, 2,
                            &sensorTaskHandle, 1);
    xTaskCreatePinnedToCore(diagnosticsTask, "Diagnostico", 3072, nullptr, 1,
                            &diagnosticsTaskHandle, 1);
    // Cada monitor queda fijado al nucleo que mide.
    xTaskCreatePinnedToCore(cpuMonitorTask, "CPU0", 2048,
                            reinterpret_cast<void *>(0), 1, nullptr, 0);
    xTaskCreatePinnedToCore(cpuMonitorTask, "CPU1", 2048,
                            reinterpret_cast<void *>(1), 1, nullptr, 1);
}

void loop()
{
    // El trabajo real ya esta distribuido entre tareas FreeRTOS.
    vTaskDelay(portMAX_DELAY);
}