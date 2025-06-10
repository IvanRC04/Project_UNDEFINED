#include <Arduino.h>
#include "src/quirc/quirc.h"
#include "esp_camera.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
// Añade al inicio del archivo
#include <freertos/queue.h>
#include <freertos/task.h>
// WiFi
const char *ssid = "iPhoneRoman";
const char *password = "RomanAAA";

// MQTT Broker
const char *mqtt_broker = "broker.hivemq.com";
const char *topic_pedidos = "tienda/pedidos";
const char *topic_respuesta = "tienda/respuesta";
const int mqtt_port = 1883;

// Tamaño de la cola circular
#define QUEUE_SIZE 10

// Pines para LEDs
#define LED_AMARILLO 47  // GPIO14 para LED amarillo
#define LED_VERDE    48  // GPIO15 para LED verde

// Añade estas definiciones al inicio con las otras
#define BOTON_EMERGENCIA 14  // GPIO14 para botón de emergencia (pull-up interno)
#define BOTON_REANUDAR   20  // GPIO20 para botón de reanudar (pull-up interno)
#define BUZZER_PIN       19  // GPIO19 para el buzzer
// Estructura para la cola circular
struct Pedido {
  String mensaje;
  bool procesado;
};

Pedido colaPedidos[QUEUE_SIZE];
int frente = 0;
int final = 0;
int contador = 0;

// Variables globales
WiFiClient espClient;
PubSubClient client(espClient);
TaskHandle_t QRCodeReader_Task;
String QRCodeResult = "no";
bool esperandoQR = false;
bool qrLeido = false;

/* Configuración de pines para CAMERA_MODEL_ESP32S3_EYE */
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

// Variables para el lector QR
struct quirc *q = NULL;
uint8_t *image = NULL;
camera_fb_t *fb = NULL;
struct quirc_code code;
struct quirc_data data;
quirc_decode_error_t err;

// Mutex para protección de la cola
SemaphoreHandle_t xMutex;

// Definición de pines para el motor paso a paso (ESP32-S3)
#define IN1 1  
#define IN2 2 
#define IN3 3  
#define IN4 21  

// Secuencias de pasos (modo onda completa)
const int pasosAntihorario[4] = {B1001, B0011, B0110, B1100}; // Sentido antihorario

// Prototipos de funciones para el motor
void ejecutarPaso(int paso);
void detenerMotor();
void girarUnaVueltaAntihorario();


// Añade esta función para manejar la reanudación
void manejarReanudar() {
    // Pequeño beep de confirmación
    tone(BUZZER_PIN, 800, 200); // 800Hz por 200ms
    
    // Crear mensaje JSON de reanudación
    DynamicJsonDocument doc(128);
    doc["tipo"] = "reanudar";
    
    String mensaje;
    serializeJson(doc, mensaje);
    
    // Publicar mensaje de reanudación
    if (client.connected()) {
        if (client.publish(topic_pedidos, mensaje.c_str())) {
            Serial.println("Mensaje de reanudación enviado");
        } else {
            Serial.println("Error al enviar reanudación");
        }
    } else {
        Serial.println("No conectado MQTT - no se pudo enviar reanudación");
    }
    
    // Parpadear LED verde 2 veces para confirmación
    for(int i = 0; i < 2; i++) {
        digitalWrite(LED_VERDE, HIGH);
        delay(200);
        digitalWrite(LED_VERDE, LOW);
        delay(200);
    }
}

// Añade esta función para manejar la emergencia
void manejarEmergencia() {
    // Activar buzzer (tono de emergencia)
    tone(BUZZER_PIN, 1000, 1000); // 1kHz por 1 segundo
    
    // Crear mensaje JSON de emergencia
    DynamicJsonDocument doc(128);
    doc["tipo"] = "emergencia";
    
    String mensaje;
    serializeJson(doc, mensaje);
    
    // Publicar mensaje de emergencia
    if (client.connected()) {
        if (client.publish(topic_pedidos, mensaje.c_str())) {
            Serial.println("Mensaje de emergencia enviado");
        } else {
            Serial.println("Error al enviar emergencia");
        }
    } else {
        Serial.println("No conectado MQTT - no se pudo enviar emergencia");
    }
    
    // Encender LEDs para indicar emergencia
    digitalWrite(LED_AMARILLO, HIGH);
    digitalWrite(LED_VERDE, HIGH);
    delay(1000);
    digitalWrite(LED_AMARILLO, LOW);
    digitalWrite(LED_VERDE, LOW);
    
    // Detener motor por si estaba en movimiento
    detenerMotor();
}


// Función para encolar un nuevo pedido
bool encolarPedido(String mensaje) {
  xSemaphoreTake(xMutex, portMAX_DELAY);
  
  if (contador == QUEUE_SIZE) {
    xSemaphoreGive(xMutex);
    return false; // Cola llena
  }
  
  colaPedidos[final].mensaje = mensaje;
  colaPedidos[final].procesado = false;
  final = (final + 1) % QUEUE_SIZE;
  contador++;
  
  xSemaphoreGive(xMutex);
  return true;
}

// Función para desencolar un pedido
bool desencolarPedido(String &mensaje) {
  xSemaphoreTake(xMutex, portMAX_DELAY);
  
  if (contador == 0) {
    xSemaphoreGive(xMutex);
    return false; // Cola vacía
  }
  
  mensaje = colaPedidos[frente].mensaje;
  colaPedidos[frente].procesado = true;
  frente = (frente + 1) % QUEUE_SIZE;
  contador--;
  
  xSemaphoreGive(xMutex);
  return true;
}

// Función para obtener el primer pedido sin procesar
bool obtenerProximoPedido(String &mensaje) {
  xSemaphoreTake(xMutex, portMAX_DELAY);
  
  if (contador == 0) {
    xSemaphoreGive(xMutex);
    return false; // Cola vacía
  }
  
  // Buscar el primer pedido no procesado
  for (int i = 0; i < QUEUE_SIZE; i++) {
    int index = (frente + i) % QUEUE_SIZE;
    if (!colaPedidos[index].procesado && !colaPedidos[index].mensaje.isEmpty()) {
      mensaje = colaPedidos[index].mensaje;
      xSemaphoreGive(xMutex);
      return true;
    }
  }
  
  xSemaphoreGive(xMutex);
  return false;
}

// Función para ver si hay pedidos pendientes
bool hayPedidosPendientes() {
  xSemaphoreTake(xMutex, portMAX_DELAY);
  bool resultado = (contador > 0);
  xSemaphoreGive(xMutex);
  return resultado;
}

void callback(char* topicName, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Mensaje recibido en topic ");
  Serial.println(topicName);
  Serial.print("Contenido: ");
  Serial.println(message);

  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, message);
  
  if (error) {
    Serial.println("Error: Mensaje JSON no válido");
    return;
  }

  // Procesamiento de mensajes en tienda/respuesta (debe ir primero)
  if (String(topicName) == topic_respuesta) {
    if (doc.containsKey("estado")) {
      if (doc["estado"] == "fin") {
        Serial.println("====================================");
        Serial.println("| Mensaje especial recibido: 'fin' |");
        Serial.println("| Activando motor paso a paso      |");
        Serial.println("====================================");
        
        // Apagar LED amarillo y encender verde
        digitalWrite(LED_AMARILLO, LOW);
        digitalWrite(LED_VERDE, HIGH);
        
        girarUnaVueltaAntihorario(); // Gira el motor una vuelta
        
        // Esperar 4 segundos con LED verde encendido
        delay(4000);
        digitalWrite(LED_VERDE, LOW);
      } else {
        Serial.println("Mensaje de respuesta recibido pero estado no es 'fin'");
      }
    } else {
      Serial.println("Mensaje en tienda/respuesta no contiene campo 'estado'");
    }
    return; // Salir después de procesar respuesta
  }

  // Procesamiento de mensajes en tienda/pedidos
  if (String(topicName) == topic_pedidos) {
    if (!doc.containsKey("talla") || !doc.containsKey("color") || !doc.containsKey("tipo")) {
      Serial.println("Error: Mensaje no tiene el formato correcto (faltan campos)");
      return;
    }
    
    if (doc["tipo"] == "no") {
      if (!encolarPedido(message)) {
        Serial.println("Cola de pedidos llena, descartando mensaje");
      } else {
        Serial.println("Pedido añadido a la cola");
      }
    } else {
      Serial.println("Mensaje recibido pero tipo no es 'no', ignorando");
    }
  }
}

// Funciones para controlar el motor
void girarUnaVueltaAntihorario() {
  Serial.println("Girando motor una vuelta antihoraria...");
  for (int i = 0; i < 512; i++) { // 512 ciclos * 4 pasos = 2048 pasos (1 vuelta)
    for (int j = 0; j < 4; j++) {
      ejecutarPaso(pasosAntihorario[j]);
      delay(5); // Ajusta la velocidad (menos delay = más rápido)
    }
  }
  detenerMotor();
  Serial.println("Motor detenido");
}

void ejecutarPaso(int paso) {
  digitalWrite(IN1, bitRead(paso, 0));
  digitalWrite(IN2, bitRead(paso, 1));
  digitalWrite(IN3, bitRead(paso, 2));
  digitalWrite(IN4, bitRead(paso, 3));
}

void detenerMotor() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void setup() {
  // Configurar pines de motor
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  detenerMotor(); // Asegura que el motor está detenido al inicio

  // Configurar pines de LEDs
  pinMode(LED_AMARILLO, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  digitalWrite(LED_AMARILLO, LOW);
  digitalWrite(LED_VERDE, LOW);

  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  pinMode(BOTON_EMERGENCIA, INPUT_PULLUP); // Botón emergencia con pull-up interno
  pinMode(BOTON_REANUDAR, INPUT_PULLUP);   // Botón reanudar con pull-up interno
  pinMode(BUZZER_PIN, OUTPUT);             // Buzzer como salida
  digitalWrite(BUZZER_PIN, LOW);           // Asegurar que está apagado
    
  Serial.println("Configuración de emergencia lista");

  // Inicializar mutex
  xMutex = xSemaphoreCreateMutex();

  // Conexión WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println("Connecting to WiFi..");
  }
  Serial.println("Connected to the Wi-Fi network");

  // Configuración MQTT
  client.setServer(mqtt_broker, mqtt_port);
  client.setCallback(callback);

  while (!client.connected()) {
    String client_id = "esp32-client-";
    client_id += String(WiFi.macAddress());
    Serial.printf("The client %s connects to the public MQTT broker\n", client_id.c_str());
    if (client.connect(client_id.c_str())) {
      Serial.println("Public EMQX MQTT broker connected");
      client.subscribe(topic_pedidos);
      client.subscribe(topic_respuesta); // Nueva suscripción
      Serial.println("Suscrito a los topics:");
      Serial.println("- " + String(topic_pedidos));
      Serial.println("- " + String(topic_respuesta));
    } else {
      Serial.print("failed with state ");
      Serial.print(client.state());
      delay(2000);
    }
  }

  // Configuración de la cámara
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_GRAYSCALE;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 15;
  config.fb_count = 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    ESP.restart();
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);

  Serial.println("Cámara configurada correctamente");

  // Crear tarea para el lector QR
  xTaskCreatePinnedToCore(
    QRCodeReader,
    "QRCodeReader_Task",
    10000,
    NULL,
    1,
    &QRCodeReader_Task,
    0);
}

void loop() {
  client.loop();

      // Verificar botón de emergencia (está en LOW cuando se presiona por el pull-up)
    if (digitalRead(BOTON_EMERGENCIA) == LOW) {
        Serial.println("Botón de emergencia presionado!");
        manejarEmergencia();
        delay(1000); // Debounce y evitar múltiples activaciones
    }
    // Verificar botón de reanudar (está en LOW cuando se presiona por el pull-up)
    if (digitalRead(BOTON_REANUDAR) == LOW) {
        Serial.println("Botón de reanudar presionado!");
        manejarReanudar();
        delay(1000); // Debounce y evitar múltiples activaciones
    }
  
  // Procesar siguiente pedido si no estamos esperando un QR
  if (!esperandoQR && hayPedidosPendientes()) {
    String mensajePedido;
    if (obtenerProximoPedido(mensajePedido)) {
      procesarPedido(mensajePedido);
    }
  }
  
  // Si hemos leído un QR, procesarlo
  if (qrLeido) {
    enviarPedidoCompleto();
    vTaskDelay(10000 / portTICK_PERIOD_MS);
    qrLeido = false;
    esperandoQR = false;
  }
  
  delay(10);
}



//rarillo esta funcion
void procesarPedido(String mensajePedido) {
  Serial.println("Iniciando procesamiento de pedido...");
  esperandoQR = true;
  QRCodeResult = "no"; // Resetear el resultado del QR
  digitalWrite(LED_AMARILLO, HIGH); // Encender LED amarillo

}



void QRCodeReader(void *pvParameters) {
  Serial.println("Lector QR listo");
  Serial.print("Tarea QR corriendo en core ");
  Serial.println(xPortGetCoreID());

  while(1) {
    if (esperandoQR) {
      q = quirc_new();
      if (!q) {
        Serial.println("Error al crear objeto quirc");
        vTaskDelay(100 / portTICK_PERIOD_MS);
        continue;
      }

      fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("Error al capturar imagen");
        quirc_destroy(q);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        continue;
      }

      quirc_resize(q, fb->width, fb->height);
      image = quirc_begin(q, NULL, NULL);
      if (!image) {
        Serial.println("Error al obtener buffer de imagen");
        esp_camera_fb_return(fb);
        quirc_destroy(q);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        continue;
      }
      
      memcpy(image, fb->buf, fb->len);
      quirc_end(q);

      int count = quirc_count(q);
      if (count > 0) {
        quirc_extract(q, 0, &code);
        err = quirc_decode(&code, &data);

        if (err) {
          Serial.print("Error al decodificar QR: ");
          Serial.println(quirc_strerror(err));
        } else {
          Serial.println("QR decodificado correctamente");
          QRCodeResult = String((const char *)data.payload);
          qrLeido = true;
        }
      }

      esp_camera_fb_return(fb);
      fb = NULL;
      image = NULL;
      quirc_destroy(q);
      vTaskDelay(100 / portTICK_PERIOD_MS);
    } else {
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }
  }
}

void enviarPedidoCompleto() {
  String mensajePedido;
  
  // Buscar el primer pedido no procesado
  if (!obtenerProximoPedido(mensajePedido)) {
    Serial.println("Error: No hay pedido para enviar");
    return;
  }

  // Parsear el JSON original
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, mensajePedido);

  if (error) {
    Serial.print("Error al parsear JSON: ");
    Serial.println(error.c_str());
    return;
  }

  // Actualizar solo el campo "tipo"
  doc["tipo"] = QRCodeResult;

  // Crear el mensaje final
  String mensajeFinal;
  serializeJson(doc, mensajeFinal);

  // Publicar el mensaje
  if (client.connected()) {
    if (client.publish(topic_pedidos, mensajeFinal.c_str())) {
      Serial.print("Mensaje publicado: ");
      Serial.println(mensajeFinal);
      
      // Marcar el pedido como procesado
      xSemaphoreTake(xMutex, portMAX_DELAY);
      for (int i = 0; i < QUEUE_SIZE; i++) {
        int index = (frente + i) % QUEUE_SIZE;
        if (colaPedidos[index].mensaje == mensajePedido) {
          colaPedidos[index].procesado = true;
          break;
        }
      }
      xSemaphoreGive(xMutex);
    } else {
      Serial.println("Error al publicar el mensaje");
    }
  } else {
    Serial.println("Error: No conectado a MQTT");
    // No reencolamos porque el pedido sigue estando en la cola como no procesado
  }
}