# 🚗💤 DriverWatch

> Sistema de detección de somnolencia en conductores en tiempo real mediante visión artificial, IoT y actuación física.

---

## 📋 Descripción

**DriverWatch** es un sistema mecatrónico de monitoreo inteligente de conductores desarrollado como proyecto de innovación tecnológica en el Instituto Tecnológico Superior de la Región de los Llanos (ITSRLL). Detecta señales de fatiga, somnolencia y bostezo en tiempo real mediante visión por computadora, genera alertas físicas y registra todos los eventos en una base de datos para análisis posterior.

El sistema está orientado a conductores de transporte pesado, repartidores y operadores de flotillas que realizan jornadas prolongadas de manejo.

---

## 👥 Equipo de desarrollo

| Nombre | Control |
|---|---|
| Alejandra Berenice Flores García | 22030008 |
| Esmeralda Gómez Huerta | 22030010 |
| Jean Paul Acosta Navarro | 22030001 |
| Eber Jafet Rodríguez Valenciano | 22030004 |

**Docente:** Osbaldo Aragón Banderas  
**Materia:** Programación Avanzada — Ingeniería Mecatrónica  
**Institución:** ITSRLL — Período ENE-JUN 2026

---

## 🏗️ Arquitectura del sistema

```
┌──────────┐  video 640×480 px   ┌──────────────────────────────────┐
│  CÁMARA  │ ──────────────────► │  camara.py  (Python / MediaPipe) │
│  USB     │                     │  EAR < 0.18 → cansado            │
└──────────┘                     │  MAR > 0.75 → bostezo            │
                                 └────────────┬─────────────────────┘
                                              │ MQTT pub → rostro/faccion
                                 ┌────────────▼─────────────────────┐
                                 │  HiveMQ Broker :1883             │
                                 └────────────┬─────────────────────┘
                                              │ MQTT sub
                                 ┌────────────▼─────────────────────┐
                                 │  Node-RED (middleware + dashboard)│
                                 │  Construye JSON → SQLite          │
                                 └────────────┬─────────────────────┘
                                              │ MQTT pub → driverwatch/comando
                      ┌── Serial COM9 (fallback) ──┘
                 ┌────▼──────────────────────────────┐
                 │  ESP32-C3  (firmware C++ / FSM)    │
                 │  GPIO 4 → BUZZER                   │
                 │  NVS Flash → cola 20 eventos       │
                 └───────────────────────────────────┘
```

---

## ⚙️ Tecnologías utilizadas

### Hardware
| Componente | Función |
|---|---|
| ESP32-C3 | Microcontrolador principal — FSM, MQTT, actuación |
| Buzzer piezoeléctrico | Alerta sonora en GPIO 4 |
| Cámara USB / WebCam | Captura de video del conductor |
| PC / Laptop (CPU i5) | Procesamiento de visión artificial |

### Software
| Tecnología | Uso |
|---|---|
| Python 3 + MediaPipe | Detección facial y cálculo EAR/MAR |
| OpenCV | Captura y procesamiento de frames |
| paho-mqtt | Comunicación MQTT desde Python |
| C++ / Arduino Framework | Firmware del ESP32-C3 |
| ArduinoJson | Parsing de comandos JSON en ESP32 |
| Node-RED | Middleware IoT y dashboard de monitoreo |
| HiveMQ | Broker MQTT público (broker.hivemq.com:1883) |
| SQLite | Base de datos local de eventos |

---

## 🔍 Detección de somnolencia

El sistema calcula dos métricas biomecánicas a partir de los 478 landmarks de MediaPipe Face Landmarker:

### Eye Aspect Ratio (EAR)
Detecta cierre prolongado de ojos:
```
EAR = (|p2-p6| + |p3-p5|) / (2 × |p1-p4|)
```
- **Umbral:** `EAR < 0.18` durante **12 frames consecutivos** → estado `cansado`

### Mouth Aspect Ratio (MAR)
Detecta bostezos:
```
MAR = |upper-lower| / |left-right|
```
- **Umbral:** `MAR > 0.75` durante **8 frames consecutivos** → estado `bostezo`

### Estados del conductor

| Estado | Condición | Acción del sistema |
|---|---|---|
| `estable` | EAR ≥ 0.18 y MAR ≤ 0.75 | Monitoreo continuo |
| `cansado` | EAR < 0.18 por ≥ 12 frames | Alerta MQTT + buzzer 1200 ms |
| `bostezo` | MAR > 0.75 por ≥ 8 frames | Alerta MQTT + buzzer 1200 ms |
| `sin_rostro` | No se detectan landmarks | Publicación de evento |

---

## 📡 Comunicación MQTT

| Topic | Dirección | Descripción |
|---|---|---|
| `rostro/faccion` | Python → Node-RED | Facción detectada |
| `driverwatch/comando` | Node-RED → ESP32-C3 | Comando JSON de actuación |
| `driverwatch/estado` | ESP32-C3 → broker | Telemetría FSM (heartbeat cada 5 s) |
| `driverwatch/eventos` | ESP32-C3 → broker | Eventos procesados con estado_fsm |
| `driverwatch/fallback` | ESP32-C3 → broker | Eventos NVS reenviados al reconectar |

### Estructura del JSON de comando
```json
{
  "cmd":         "guardar_evento",
  "fuente":      "python",
  "faccion":     "bostezo",
  "confianza":   0.95,
  "timestamp":   "2026-05-09T10:00:00",
  "buzzer":      true,
  "duracion_ms": 1200
}
```

---

## 🤖 Máquina de Estados Finita (FSM) — ESP32-C3

```
[INICIO/RESET]
      │
      ▼
 WIFI_DOWN ──── WiFi OK ────► MQTT_DOWN ──── MQTT OK ────► IDLE
      ▲                            ▲                          │
      │                            │                   JSON recibido
      │                    MQTT desconectado                  │
      │                            │                          ▼
      │                            │                  PROCESS_COMMAND
      │                            │                  ┌───────┴───────┐
      │                            │              buzzer=true    buzzer=false
      │                            │                  │               │
      │                            │                  ▼               ▼
      │                            │             ACTUATING          IDLE
      │                            │                  │
      │                            │           timer expirado
      │                            │                  │
      │                     ┌──────▼──────┐           │
      │                     │SYNC_PENDING │◄──────────┘
      │                     └──────┬──────┘
      │                     enviado OK
      │                            │
      └──── WiFi perdido ──────────┘
```

---

## 📁 Estructura del repositorio

```
DriverWatch/
│
├── camara.py                        # Módulo de visión artificial (Python)
├── main.cpp                         # Firmware ESP32-C3 (C++/Arduino)
├── DriverWatch.json                 # Flujo Node-RED exportado (44 nodos)
├── driverwatch_eventos_reales.db    # Base de datos SQLite con 2,183 eventos reales
├── driverwatch.ipynb                # Notebook ML: clasificación de estados de fatiga
├── models/
│   └── face_landmarker.task         # Modelo MediaPipe Face Landmarker
├── docs/
│   └── U3A2_Gomez_Esmeralda.docx   # Reporte técnico Etapa 2
└── README.md
```

---

## 🚀 Instalación y uso

### Requisitos previos
```bash
pip install opencv-python mediapipe paho-mqtt pyserial
```

### 1. Configurar el firmware ESP32-C3
Abrir `main.cpp` y actualizar las credenciales Wi-Fi:
```cpp
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_CONTRASEÑA";
```
Flashear con PlatformIO o Arduino IDE al ESP32-C3.

### 2. Importar el flujo Node-RED
1. Abrir Node-RED (`http://localhost:1880`)
2. Menú → Import → pegar contenido de `DriverWatch.json`
3. Deploy

### 3. Ejecutar el módulo de visión
```bash
python camara.py
```
> La cámara USB debe estar en el índice 1. Si no abre, cambiar `CAMERA_INDEX = 0` en `camara.py`.

### 4. Verificar en el dashboard
Abrir `http://localhost:1880/ui` para ver el monitoreo en tiempo real.

---

## 📊 Métricas de rendimiento

| Métrica | Valor |
|---|---|
| Latencia MediaPipe típica | 10–12 ms |
| Latencia MediaPipe pico | 27–34 ms |
| Intervalo publicación MQTT | 1.5 s (deduplicación) |
| Capacidad cola NVS Fallback | 20 eventos |
| Heartbeat ESP32 al broker | Cada 5 s |
| Registros en BD SQLite | 2,183+ eventos reales |
| Tiempo reintento Wi-Fi | 5 s |
| Tiempo reintento MQTT | 4 s |

---

## 📄 Licencia

Proyecto académico — Instituto Tecnológico Superior de la Región de los Llanos  
Período ENE-JUN 2026 — Ingeniería Mecatrónica
