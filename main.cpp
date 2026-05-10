#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// =====================================================
// CONFIGURACIÓN
// =====================================================
const char* WIFI_SSID     = "Tacos de pla";
const char* WIFI_PASSWORD = "vivamiura";

const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "esp32c3_driverwatch_001";

const char* TOPIC_CMD     = "driverwatch/comando";
const char* TOPIC_STATUS  = "driverwatch/estado";
const char* TOPIC_EVENTS  = "driverwatch/eventos";
const char* TOPIC_FALLBACK = "driverwatch/fallback";

const int PIN_BUZZER = 4;

// =====================================================
// FSM
// =====================================================
enum EstadoFSM {
  IDLE,
  WIFI_DOWN,
  MQTT_DOWN,
  PROCESS_COMMAND,
  ACTUATING,
  SYNC_PENDING,
  ERROR_STATE
};

EstadoFSM estadoActual = IDLE;

// =====================================================
// COLA DE EVENTOS
// =====================================================
struct EventoPendiente {
  bool ocupado = false;
  String json = "";
};

const int MAX_EVENTOS = 20;
EventoPendiente cola[MAX_EVENTOS];

// =====================================================
// OBJETOS GLOBALES
// =====================================================
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
Preferences prefs;

// =====================================================
// VARIABLES
// =====================================================
unsigned long lastWifiAttempt   = 0;
unsigned long lastMqttAttempt   = 0;
unsigned long lastStatusPublish = 0;
unsigned long buzzerStartMs     = 0;
unsigned long buzzerDuracionMs  = 0;

bool buzzerActivo = false;
bool hayComandoPendiente = false;
String comandoPendiente = "";

String serialBuffer = "";

// =====================================================
// UTILIDADES
// =====================================================
const char* estadoToStr(EstadoFSM e) {
  switch (e) {
    case IDLE:            return "IDLE";
    case WIFI_DOWN:       return "WIFI_DOWN";
    case MQTT_DOWN:       return "MQTT_DOWN";
    case PROCESS_COMMAND: return "PROCESS_COMMAND";
    case ACTUATING:       return "ACTUATING";
    case SYNC_PENDING:    return "SYNC_PENDING";
    case ERROR_STATE:     return "ERROR_STATE";
    default:              return "UNKNOWN";
  }
}

void cambiarEstado(EstadoFSM nuevo) {
  if (estadoActual != nuevo) {
    Serial.print("[FSM] ");
    Serial.print(estadoToStr(estadoActual));
    Serial.print(" -> ");
    Serial.println(estadoToStr(nuevo));
    estadoActual = nuevo;
  }
}

void publicarEstado(const String& detalle) {
  if (!mqttClient.connected()) return;

  StaticJsonDocument<256> doc;
  doc["estado"] = estadoToStr(estadoActual);
  doc["detalle"] = detalle;
  doc["wifi"] = (WiFi.status() == WL_CONNECTED);
  doc["mqtt"] = mqttClient.connected();
  doc["ip"] = WiFi.localIP().toString();

  String out;
  serializeJson(doc, out);
  mqttClient.publish(TOPIC_STATUS, out.c_str());
}

// =====================================================
// BUZZER NO BLOQUEANTE
// =====================================================
void buzzerOn() {
  digitalWrite(PIN_BUZZER, HIGH);
  buzzerActivo = true;
  buzzerStartMs = millis();
}

void buzzerOff() {
  digitalWrite(PIN_BUZZER, LOW);
  buzzerActivo = false;
  buzzerDuracionMs = 0;
}

void iniciarBuzzerNoBloqueante(unsigned long duracion) {
  buzzerDuracionMs = duracion;
  buzzerOn();
}

void actualizarBuzzer() {
  if (buzzerActivo && buzzerDuracionMs > 0) {
    if (millis() - buzzerStartMs >= buzzerDuracionMs) {
      buzzerOff();
      Serial.println("Buzzer apagado automaticamente");
    }
  }
}

// =====================================================
// COLA LOCAL EN FLASH
// =====================================================
void limpiarColaRAM() {
  for (int i = 0; i < MAX_EVENTOS; i++) {
    cola[i].ocupado = false;
    cola[i].json = "";
  }
}

void guardarColaEnFlash() {
  prefs.begin("cola", false);
  for (int i = 0; i < MAX_EVENTOS; i++) {
    String key = "e" + String(i);
    if (cola[i].ocupado) {
      prefs.putString(key.c_str(), cola[i].json);
    } else {
      prefs.putString(key.c_str(), "");
    }
  }
  prefs.end();
}

void cargarColaDesdeFlash() {
  prefs.begin("cola", true);
  for (int i = 0; i < MAX_EVENTOS; i++) {
    String key = "e" + String(i);
    String valor = prefs.getString(key.c_str(), "");
    if (valor.length() > 0) {
      cola[i].ocupado = true;
      cola[i].json = valor;
    } else {
      cola[i].ocupado = false;
      cola[i].json = "";
    }
  }
  prefs.end();
}

bool encolarEvento(const String& json) {
  for (int i = 0; i < MAX_EVENTOS; i++) {
    if (!cola[i].ocupado) {
      cola[i].ocupado = true;
      cola[i].json = json;
      guardarColaEnFlash();

      Serial.println("Evento guardado localmente:");
      Serial.println(json);
      return true;
    }
  }

  Serial.println("ERROR: Cola local llena.");
  return false;
}

bool hayEventosPendientes() {
  for (int i = 0; i < MAX_EVENTOS; i++) {
    if (cola[i].ocupado) return true;
  }
  return false;
}

bool enviarPrimerPendiente() {
  if (!mqttClient.connected()) return false;

  for (int i = 0; i < MAX_EVENTOS; i++) {
    if (cola[i].ocupado) {
      bool ok = mqttClient.publish(TOPIC_EVENTS, cola[i].json.c_str());
      if (ok) {
        Serial.print("Reenviado pendiente #");
        Serial.print(i);
        Serial.print(": ");
        Serial.println(cola[i].json);

        cola[i].ocupado = false;
        cola[i].json = "";
        guardarColaEnFlash();
        return true;
      } else {
        Serial.println("No se pudo reenviar pendiente.");
        return false;
      }
    }
  }

  return false;
}

void imprimirEventosGuardados() {
  Serial.println("===== EVENTOS GUARDADOS =====");
  bool hay = false;

  for (int i = 0; i < MAX_EVENTOS; i++) {
    if (cola[i].ocupado) {
      hay = true;
      Serial.print("#");
      Serial.print(i);
      Serial.print(": ");
      Serial.println(cola[i].json);
    }
  }

  if (!hay) {
    Serial.println("No hay eventos guardados.");
  }

  Serial.println("=============================");
}

void publicarEventosGuardadosMQTT() {
  if (!mqttClient.connected()) return;

  int total = 0;
  for (int i = 0; i < MAX_EVENTOS; i++) {
    if (cola[i].ocupado) total++;
  }

  // Publicar resumen con total
  StaticJsonDocument<128> resumen;
  resumen["tipo"] = "resumen_fallback";
  resumen["total"] = total;
  String resumenStr;
  serializeJson(resumen, resumenStr);
  mqttClient.publish(TOPIC_FALLBACK, resumenStr.c_str());
  delay(50);

  if (total == 0) {
    StaticJsonDocument<128> vacio;
    vacio["tipo"] = "fallback_vacio";
    vacio["mensaje"] = "No hay eventos guardados";
    String vacioStr;
    serializeJson(vacio, vacioStr);
    mqttClient.publish(TOPIC_FALLBACK, vacioStr.c_str());
    return;
  }

  // Publicar cada evento individualmente
  for (int i = 0; i < MAX_EVENTOS; i++) {
    if (cola[i].ocupado) {
      // Parsear y agregar indice
      StaticJsonDocument<512> doc;
      deserializeJson(doc, cola[i].json);
      doc["indice"] = i;
      doc["tipo"] = "evento_fallback";
      String out;
      serializeJson(doc, out);
      mqttClient.publish(TOPIC_FALLBACK, out.c_str());
      delay(50);
    }
  }
}

// =====================================================
// WIFI / MQTT
// =====================================================
void intentarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttempt < 5000) return;

  lastWifiAttempt = millis();
  Serial.println("Intentando conectar WiFi...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String mensaje;
  for (unsigned int i = 0; i < length; i++) {
    mensaje += (char)payload[i];
  }

  Serial.print("MQTT recibido [");
  Serial.print(topic);
  Serial.print("] -> ");
  Serial.println(mensaje);

  if (String(topic) == TOPIC_CMD) {
    comandoPendiente = mensaje;
    hayComandoPendiente = true;
    cambiarEstado(PROCESS_COMMAND);
  }
}

void intentarMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  if (millis() - lastMqttAttempt < 4000) return;

  lastMqttAttempt = millis();

  Serial.println("Intentando conectar MQTT...");
  if (mqttClient.connect(MQTT_CLIENT)) {
    Serial.println("MQTT conectado");
    mqttClient.subscribe(TOPIC_CMD);
    publicarEstado("suscrito a comandos");
  } else {
    Serial.print("Fallo MQTT rc=");
    Serial.println(mqttClient.state());
  }
}

// =====================================================
// PROCESAMIENTO JSON
// =====================================================
void procesarComandoJSON(const String& jsonCmd) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonCmd);

  if (err) {
    Serial.print("Error parseando JSON: ");
    Serial.println(err.c_str());
    cambiarEstado(ERROR_STATE);
    publicarEstado("json_invalido");
    return;
  }

  const char* cmd        = doc["cmd"]        | "";
  const char* faccion    = doc["faccion"]    | "desconocida";
  float confianza        = doc["confianza"]  | 0.0;
  const char* timestamp  = doc["timestamp"]  | "";
  bool buzzer            = doc["buzzer"]     | false;
  int duracion_ms        = doc["duracion_ms"]| 1000;
  const char* fuente     = doc["fuente"]     | "desconocida";

  Serial.println("=== Comando parseado ===");
  Serial.print("cmd: ");        Serial.println(cmd);
  Serial.print("faccion: ");    Serial.println(faccion);
  Serial.print("confianza: ");  Serial.println(confianza);
  Serial.print("timestamp: ");  Serial.println(timestamp);
  Serial.print("buzzer: ");     Serial.println(buzzer ? "true" : "false");
  Serial.print("duracion_ms: ");Serial.println(duracion_ms);
  Serial.print("fuente: ");     Serial.println(fuente);

  if (String(cmd) == "guardar_evento") {
    if (buzzer) {
      iniciarBuzzerNoBloqueante((unsigned long)duracion_ms);
      cambiarEstado(ACTUATING);
      Serial.println("Buzzer activado");
    }

    StaticJsonDocument<512> evento;
    evento["origen"]     = "esp32c3";
    evento["fuente"]     = fuente;
    evento["faccion"]    = faccion;
    evento["confianza"]  = confianza;
    evento["timestamp"]  = timestamp;
    evento["buzzer"]     = buzzer;
    evento["duracion_ms"]= duracion_ms;
    evento["estado_fsm"] = estadoToStr(estadoActual);

    String out;
    serializeJson(evento, out);

    if (mqttClient.connected()) {
      bool ok = mqttClient.publish(TOPIC_EVENTS, out.c_str());
      if (ok) {
        Serial.println("Evento enviado al servidor:");
        Serial.println(out);
        publicarEstado("evento_enviado");
      } else {
        Serial.println("No se pudo publicar. Guardando localmente...");
        encolarEvento(out);
        publicarEstado("evento_guardado_local");
      }
    } else {
      Serial.println("Sin conexion MQTT. Guardando localmente...");
      encolarEvento(out);
      publicarEstado("evento_guardado_local");
    }

    if (!buzzer) {
      cambiarEstado(IDLE);
    }
  }
  else if (String(cmd) == "buzzer_on") {
    iniciarBuzzerNoBloqueante((unsigned long)duracion_ms);
    Serial.println("Buzzer encendido manualmente");
    publicarEstado("buzzer_activado");
    cambiarEstado(ACTUATING);
  }
  else if (String(cmd) == "buzzer_off") {
    buzzerOff();
    Serial.println("Buzzer apagado manualmente");
    publicarEstado("buzzer_apagado");
    cambiarEstado(IDLE);
  }
  else if (String(cmd) == "mostrar_guardados") {
    imprimirEventosGuardados();
    publicarEventosGuardadosMQTT();
    publicarEstado("mostrando_guardados");
    cambiarEstado(IDLE);
  }
  else {
    Serial.println("Comando no reconocido");
    publicarEstado("cmd_no_reconocido");
    cambiarEstado(ERROR_STATE);
  }
}

// =====================================================
// ENTRADA SERIAL
// =====================================================
void leerEntradaSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\n') {
      serialBuffer.trim();

      if (serialBuffer.length() > 0) {
        if (serialBuffer.startsWith("{")) {
          Serial.println("JSON recibido por Serial:");
          Serial.println(serialBuffer);
          cambiarEstado(PROCESS_COMMAND);
          procesarComandoJSON(serialBuffer);
        }
        else if (serialBuffer == "show") {
          imprimirEventosGuardados();
        }
        else if (serialBuffer == "clear") {
          limpiarColaRAM();
          guardarColaEnFlash();
          Serial.println("Cola local borrada.");
        }
        else {
          Serial.print("Entrada serial no reconocida: ");
          Serial.println(serialBuffer);
          Serial.println("Usa JSON, show o clear");
        }
      }

      serialBuffer = "";
    } else {
      serialBuffer += c;
    }
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  limpiarColaRAM();
  cargarColaDesdeFlash();

  Serial.println("==================================");
  Serial.println("DriverWatch ESP32-C3 iniciado");
  Serial.println("Eventos recuperados de memoria:");
  imprimirEventosGuardados();
  Serial.println("==================================");

  WiFi.mode(WIFI_STA);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  cambiarEstado(WIFI_DOWN);
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  leerEntradaSerial();
  actualizarBuzzer();

  if (WiFi.status() != WL_CONNECTED) {
    cambiarEstado(WIFI_DOWN);
    intentarWiFi();
  } else {
    if (!mqttClient.connected()) {
      cambiarEstado(MQTT_DOWN);
      intentarMQTT();
    } else {
      mqttClient.loop();

      if (hayComandoPendiente) {
        hayComandoPendiente = false;
        cambiarEstado(PROCESS_COMMAND);
        procesarComandoJSON(comandoPendiente);
      }
      else if (hayEventosPendientes()) {
        cambiarEstado(SYNC_PENDING);
        enviarPrimerPendiente();
      }
      else if (!buzzerActivo) {
        cambiarEstado(IDLE);
      }
    }
  }

  if (!buzzerActivo && estadoActual == ACTUATING) {
    if (hayEventosPendientes()) {
      cambiarEstado(SYNC_PENDING);
    } else {
      cambiarEstado(IDLE);
    }
  }

  if (mqttClient.connected() && millis() - lastStatusPublish >= 5000) {
    lastStatusPublish = millis();
    publicarEstado("heartbeat");
  }
}