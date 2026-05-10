from pathlib import Path
from datetime import datetime
import cv2
import time
import math
import json
import threading
import serial
import mediapipe as mp
import paho.mqtt.client as mqtt

# =========================================================
# CONFIGURACIÓN GENERAL
# =========================================================
BROKER = "broker.hivemq.com"
PORT = 1883
TOPIC = "rostro/faccion"

SERIAL_PORT = "COM9"
SERIAL_BAUD = 115200

MQTT_RETRY_INTERVAL = 5
PUBLISH_INTERVAL = 1.5
PROGRAM_START_DELAY = 2.0

CAMERA_INDEX = 1
CAMERA_BACKEND = cv2.CAP_DSHOW

FRAME_WIDTH = 640
FRAME_HEIGHT = 480

MP_WIDTH = 320
MP_HEIGHT = 240
PROCESS_EVERY_N_FRAMES = 3

# =========================================================
# MQTT
# =========================================================
mqtt_connected = False
last_mqtt_attempt = 0

client = mqtt.Client()

def on_connect(client, userdata, flags, rc):
    global mqtt_connected
    mqtt_connected = (rc == 0)

    if mqtt_connected:
        print("MQTT conectado")
    else:
        print(f"Error MQTT rc={rc}")

def on_disconnect(client, userdata, rc):
    global mqtt_connected
    mqtt_connected = False
    print(f"MQTT desconectado rc={rc}")

client.on_connect = on_connect
client.on_disconnect = on_disconnect

def iniciar_mqtt():
    global last_mqtt_attempt

    try:
        print("Intentando iniciar MQTT...")
        last_mqtt_attempt = time.time()
        client.connect_async(BROKER, PORT, 60)
        client.loop_start()
    except Exception as e:
        print("No se pudo iniciar MQTT:", e)

def intentar_reconexion_mqtt():
    global last_mqtt_attempt

    if mqtt_connected:
        return

    now = time.time()
    if now - last_mqtt_attempt < MQTT_RETRY_INTERVAL:
        return

    last_mqtt_attempt = now

    try:
        print("Intentando reconectar MQTT...")
        client.reconnect()
    except Exception as e:
        print("Fallo reconexion MQTT:", e)

# =========================================================
# MEDIAPIPE
# =========================================================
BaseOptions = mp.tasks.BaseOptions
VisionRunningMode = mp.tasks.vision.RunningMode
FaceLandmarker = mp.tasks.vision.FaceLandmarker
FaceLandmarkerOptions = mp.tasks.vision.FaceLandmarkerOptions

BASE_DIR = Path(__file__).resolve().parent
MODEL_PATH = BASE_DIR / "models" / "face_landmarker.task"

print("Modelo:", MODEL_PATH)
print("Existe:", MODEL_PATH.exists())

if not MODEL_PATH.exists():
    raise FileNotFoundError(f"No se encontró el modelo en: {MODEL_PATH}")

model_bytes = MODEL_PATH.read_bytes()
print("Tamaño modelo (bytes):", len(model_bytes))

if len(model_bytes) == 0:
    raise ValueError(f"El archivo del modelo está vacío: {MODEL_PATH}")

options = FaceLandmarkerOptions(
    base_options=BaseOptions(model_asset_buffer=model_bytes),
    running_mode=VisionRunningMode.VIDEO,
    num_faces=1,
    output_face_blendshapes=False,
    output_facial_transformation_matrixes=False
)

landmarker = FaceLandmarker.create_from_options(options)
print("Landmarker inicializado correctamente")

# =========================================================
# CÁMARA
# =========================================================
print(f"Intentando abrir cámara índice {CAMERA_INDEX}...")
cap = cv2.VideoCapture(CAMERA_INDEX, CAMERA_BACKEND)

cap.set(cv2.CAP_PROP_FRAME_WIDTH, FRAME_WIDTH)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, FRAME_HEIGHT)

print("Camara abierta:", cap.isOpened())

if not cap.isOpened():
    raise RuntimeError(f"No se pudo abrir la cámara índice {CAMERA_INDEX}")

cv2.namedWindow("DriverWatch", cv2.WINDOW_NORMAL)
print("Entrando al loop principal...")

# =========================================================
# LANDMARKS
# =========================================================
LEFT_EYE  = [33, 160, 158, 133, 153, 144]
RIGHT_EYE = [362, 385, 387, 263, 373, 380]

UPPER_LIP   = 13
LOWER_LIP   = 14
MOUTH_LEFT  = 78
MOUTH_RIGHT = 308

EAR_SLEEP_THRESHOLD = 0.18
MAR_YAWN_THRESHOLD  = 0.75

SLEEP_FRAMES_REQUIRED = 12
YAWN_FRAMES_REQUIRED  = 8

sleep_counter     = 0
yawn_counter      = 0
last_sent_label   = ""
last_publish_time = 0
program_start     = time.time()

frame_count = 0
last_result = None

# =========================================================
# FUNCIONES DE CÁLCULO
# =========================================================
def distance(p1, p2):
    return math.hypot(p2[0] - p1[0], p2[1] - p1[1])

def lm_to_px(lm, w, h):
    return int(lm.x * w), int(lm.y * h)

def eye_aspect_ratio(landmarks, eye_indices, w, h):
    p1 = lm_to_px(landmarks[eye_indices[0]], w, h)
    p2 = lm_to_px(landmarks[eye_indices[1]], w, h)
    p3 = lm_to_px(landmarks[eye_indices[2]], w, h)
    p4 = lm_to_px(landmarks[eye_indices[3]], w, h)
    p5 = lm_to_px(landmarks[eye_indices[4]], w, h)
    p6 = lm_to_px(landmarks[eye_indices[5]], w, h)

    vertical_1 = distance(p2, p6)
    vertical_2 = distance(p3, p5)
    horizontal = distance(p1, p4)

    if horizontal == 0:
        return 0.0

    return (vertical_1 + vertical_2) / (2.0 * horizontal)

def mouth_aspect_ratio(landmarks, w, h):
    upper = lm_to_px(landmarks[UPPER_LIP],   w, h)
    lower = lm_to_px(landmarks[LOWER_LIP],   w, h)
    left  = lm_to_px(landmarks[MOUTH_LEFT],  w, h)
    right = lm_to_px(landmarks[MOUTH_RIGHT], w, h)

    vertical   = distance(upper, lower)
    horizontal = distance(left,  right)

    if horizontal == 0:
        return 0.0

    return vertical / horizontal

# =========================================================
# SERIAL PERSISTENTE
# =========================================================
# =========================================================
# SERIAL - abre y cierra solo cuando se necesita
# =========================================================
serial_lock = threading.Lock()

def build_serial_payload(label):
    buzzer = label in ["cansado", "bostezo"]
    return {
        "cmd":         "guardar_evento",
        "fuente":      "python",
        "faccion":     label,
        "confianza":   0.95 if label != "sin_rostro" else 0.0,
        "timestamp":   datetime.now().isoformat(timespec="seconds"),
        "buzzer":      buzzer,
        "duracion_ms": 1200 if buzzer else 0
    }

def send_serial_fallback(label):
    def _send():
        with serial_lock:
            try:
                with serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=0.5) as ser:
                    payload = build_serial_payload(label)
                    msg = json.dumps(payload)
                    ser.write((msg + "\n").encode("utf-8"))
                    ser.flush()
                print("Enviado por Serial:", msg)
            except Exception as e:
                print("Error enviando por Serial:", e)

    threading.Thread(target=_send, daemon=True).start()

# =========================================================
# PUBLICACIÓN CON FALLBACK
# =========================================================
def publish_label(label):
    global last_publish_time, last_sent_label

    now = time.time()

    if label == last_sent_label and (now - last_publish_time) < PUBLISH_INTERVAL:
        return

    enviado = False

    try:
        if mqtt_connected:
            info = client.publish(TOPIC, label)
            enviado = (info.rc == 0)

            if enviado:
                print("Publicado por MQTT:", label)

    except Exception as e:
        print("Fallo MQTT:", e)
        enviado = False

    if not enviado:
        print("Usando fallback por Serial...")
        send_serial_fallback(label)

    last_sent_label   = label
    last_publish_time = now

# =========================================================
# INICIO
# =========================================================
iniciar_mqtt()

# =========================================================
# MENSAJE DE INICIO (una sola vez antes del loop)
# =========================================================
ret, first_frame = cap.read()
if ret:
    preview = cv2.flip(first_frame, 1)
    cv2.putText(preview, "DriverWatch iniciando...", (20, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)
    cv2.imshow("DriverWatch", preview)
    cv2.waitKey(1)

# =========================================================
# LOOP PRINCIPAL
# =========================================================
while True:
    intentar_reconexion_mqtt()

    ok, frame = cap.read()
    if not ok:
        print("No se pudo leer frame")
        continue

    frame = cv2.flip(frame, 1)
    frame_count += 1

    faccion = "sin_rostro"
    ear_avg = 0.0
    mar     = 0.0

    if frame_count % PROCESS_EVERY_N_FRAMES == 0:
        try:
            small_frame = cv2.resize(frame, (MP_WIDTH, MP_HEIGHT))
            rgb         = cv2.cvtColor(small_frame, cv2.COLOR_BGR2RGB)
            mp_image    = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

            timestamp_ms = int(time.time() * 1000)

            t0          = time.time()
            last_result = landmarker.detect_for_video(mp_image, timestamp_ms)
            t1          = time.time()

            print("Tiempo MediaPipe:", round((t1 - t0) * 1000, 1), "ms")

        except Exception as e:
            print("Error en MediaPipe:", e)
            last_result = None

    result = last_result

    if result and result.face_landmarks:
        faccion   = "estable"
        landmarks = result.face_landmarks[0]

        left_ear  = eye_aspect_ratio(landmarks, LEFT_EYE,  MP_WIDTH, MP_HEIGHT)
        right_ear = eye_aspect_ratio(landmarks, RIGHT_EYE, MP_WIDTH, MP_HEIGHT)
        ear_avg   = (left_ear + right_ear) / 2.0
        mar       = mouth_aspect_ratio(landmarks, MP_WIDTH, MP_HEIGHT)

        if ear_avg < EAR_SLEEP_THRESHOLD:
            sleep_counter += 1
        else:
            sleep_counter = 0

        if mar > MAR_YAWN_THRESHOLD:
            yawn_counter += 1
        else:
            yawn_counter = 0

        if yawn_counter >= YAWN_FRAMES_REQUIRED:
            faccion = "bostezo"
        elif sleep_counter >= SLEEP_FRAMES_REQUIRED:
            faccion = "cansado"
        else:
            faccion = "estable"

        scale_x = frame.shape[1] / MP_WIDTH
        scale_y = frame.shape[0] / MP_HEIGHT

        for idx in [33, 133, 160, 144, 362, 263, 385, 380, 13, 14, 78, 308]:
            x_small, y_small = lm_to_px(landmarks[idx], MP_WIDTH, MP_HEIGHT)
            x = int(x_small * scale_x)
            y = int(y_small * scale_y)
            cv2.circle(frame, (x, y), 2, (0, 255, 255), -1)
    else:
        sleep_counter = 0
        yawn_counter  = 0

    if time.time() - program_start > PROGRAM_START_DELAY:
        publish_label(faccion)

    cv2.putText(frame, f"Faccion: {faccion}",        (20, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 255, 0), 2)
    cv2.putText(frame, f"EAR: {ear_avg:.3f}",        (20, 65),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
    cv2.putText(frame, f"MAR: {mar:.3f}",             (20, 95),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
    cv2.putText(frame, f"SleepCnt: {sleep_counter}",  (20, 125),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 200, 255), 2)
    cv2.putText(frame, f"YawnCnt: {yawn_counter}",    (20, 155),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 200, 255), 2)

    cv2.imshow("DriverWatch", frame)

    key = cv2.waitKey(10) & 0xFF
    if key == 27 or key == ord("q"):
        break

# =========================================================
# CIERRE
# =========================================================
cap.release()
cv2.destroyAllWindows()

try:
    client.loop_stop()
    client.disconnect()
except Exception:
    pass