/*
  Estacion meteorologica - ESP32
  --------------------------------
  - Lee DHT22, DS18B20 y BMP280 cada SAMPLE_INTERVAL_MS
  - Guarda cada lectura en LittleFS (flash interna), archivo /data.csv
  - Si hay internet, sincroniza lo pendiente a ThingSpeak (offline resilience)
  - Sirve una pagina web local (LAN) con graficos simples de las variables

  Librerias necesarias (Gestor de Librerias del IDE):
    - DHT sensor library (Adafruit) + Adafruit Unified Sensor
    - OneWire
    - DallasTemperature
    - Adafruit BMP280 Library
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <time.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>

// ---------------- CONFIGURACION ----------------

const char* WIFI_SSID     = "crucesita";
const char* WIFI_PASSWORD = "crucecita";
const char* THINGSPEAK_API_KEY = "GNUDCQXYFDSE62LB";

const unsigned long SAMPLE_INTERVAL_MS = 120000;  //periodo de muestreo de 2 minutos
const size_t MAX_FILE_SIZE = 100000;             // ~100KB -> recorta el historial si se pasa
const int CHART_POINTS = 100;                    // cuantos puntos se mandan al grafico web

// Zona Horaria de Argentina: GMT-3
const long GMT_OFFSET_SEC = -3 * 3600;
const int  DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER = "pool.ntp.org";

// ---------------- PINES ----------------

#define DHTPIN 4
#define DHTTYPE DHT22
#define ONE_WIRE_PIN 14
#define BMP280_ADDRESS 0x76

// conecto el dht22 al pin 4. El mismo utiliza un protocolo propio del fabricante
DHT dht(DHTPIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature ds18b20(&oneWire);
Adafruit_BMP280 bmp;
WebServer server(80);

unsigned long lastSampleTime = 0;

// Pagina HTML + JS embebidos, sin dependencias externas (funciona sin internet en la LAN)
// Se declara ACA ARRIBA (antes de setup y de handleRoot) para que el compilador
// ya la conozca cuando handleRoot() la usa mas abajo en el archivo.
const char PAGINA_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<title>Estacion Meteorologica</title>
<style>
  body { font-family: Arial, sans-serif; margin: 20px; background:#f4f4f4; }
  h1 { font-size: 1.4em; }
  .grafico { background:white; border-radius:8px; padding:10px; margin-bottom:20px; }
  canvas { width:100%; height:200px; }
</style>
</head>
<body>
<h1>Estacion Meteorologica - Datos en tiempo real</h1>

<div class="grafico"><b>Temperatura DS18B20 (C)</b><canvas id="c1"></canvas></div>
<div class="grafico"><b>Humedad (%)</b><canvas id="c2"></canvas></div>
<div class="grafico"><b>Presion (hPa)</b><canvas id="c3"></canvas></div>

<script>
function dibujarGrafico(canvasId, datos, color) {
  const canvas = document.getElementById(canvasId);
  canvas.width = canvas.clientWidth;
  canvas.height = canvas.clientHeight;
  const ctx = canvas.getContext('2d');
  ctx.clearRect(0,0,canvas.width,canvas.height);
  if (datos.length < 2) { return; }

  const min = Math.min(...datos);
  const max = Math.max(...datos);
  const rango = (max - min) || 1;
  const pad = 20;

  ctx.beginPath();
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  datos.forEach((v, i) => {
    const x = pad + (i / (datos.length - 1)) * (canvas.width - 2*pad);
    const y = canvas.height - pad - ((v - min) / rango) * (canvas.height - 2*pad);
    if (i === 0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
  });
  ctx.stroke();

  ctx.fillStyle = "#333";
  ctx.font = "11px Arial";
  ctx.fillText(max.toFixed(1), 2, pad);
  ctx.fillText(min.toFixed(1), 2, canvas.height - 5);
}

function actualizar() {
  fetch('/data').then(r => r.json()).then(d => {
    dibujarGrafico('c1', d.tempDS, '#e67e22');
    dibujarGrafico('c2', d.hum, '#3498db');
    dibujarGrafico('c3', d.pres, '#2ecc71');
  });
}

actualizar();
setInterval(actualizar, 30000); // refresca la pagina cada 30 segundos
</script>
</body>
</html>
)rawliteral";


// ---------------- ESTRUCTURA DE UNA LECTURA ----------------

struct Reading {
  time_t timestamp;
  float tempDS;
  float humedad;
  float presion;
  bool valid;
};

// =========================================================
//                        SETUP
// =========================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  setupWiFi();
  setupTime();
  setupFS();
  setupSensors();
  setupWebServer();

  Serial.println("Setup completo. Iniciando muestreo cada 2 minutos.");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastSampleTime >= SAMPLE_INTERVAL_MS || lastSampleTime == 0) {
    lastSampleTime = now;

    Reading r = readSensors();
    if (r.valid) {
      appendRecord(r);
      trimFileIfNeeded();
    } else {
      Serial.println("Lectura invalida, no se guarda este ciclo.");
    }

    // Intentar sincronizar pendientes (si hay internet, esto avanza; si no, no hace nada)
    syncPendingData();
  }
}

// =========================================================
//                     WIFI Y HORA
// =========================================================
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado. IP local: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nNo se pudo conectar al WiFi por ahora. Se reintentara solo.");
  }
}

void setupTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}

bool tiempoSincronizado() {
  time_t now = time(nullptr);
  return now > 1700000000; // fecha razonable (2023+) -> ya sincronizo por NTP
}

// =========================================================
//                  SISTEMA DE ARCHIVOS
// =========================================================
void setupFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("ERROR al montar LittleFS.");
  } else {
    Serial.println("LittleFS montado correctamente.");
  }

  if (!LittleFS.exists("/data.csv")) {
    File f = LittleFS.open("/data.csv", "w");
    if (f) f.close();
  }
  if (!LittleFS.exists("/sync_state.txt")) {
    File f = LittleFS.open("/sync_state.txt", "w");
    if (f) { f.print("0"); f.close(); }
  }
}

void appendRecord(Reading r) {
  File f = LittleFS.open("/data.csv", "a");
  if (!f) {
    Serial.println("ERROR: no se pudo abrir /data.csv para escribir.");
    return;
  }
  f.printf("%ld,%.2f,%.2f,%.2f\n",
           (long)r.timestamp, r.tempDS, r.humedad, r.presion);
  f.close();

  Serial.printf("Guardado -> ts:%ld T_DS18B20:%.2f H:%.2f P:%.2f\n",
                (long)r.timestamp, r.tempDS, r.humedad, r.presion);
}

// Si el archivo crece demasiado, nos quedamos solo con la mitad mas reciente.
void trimFileIfNeeded() {
  File f = LittleFS.open("/data.csv", "r");
  if (!f) return;
  size_t size = f.size();
  if (size <= MAX_FILE_SIZE) {
    f.close();
    return;
  }

  // Leer todas las lineas
  std::vector<String> lineas;
  while (f.available()) {
    lineas.push_back(f.readStringUntil('\n'));
  }
  f.close();

  size_t desde = lineas.size() / 2; // nos quedamos con la mitad mas nueva
  File out = LittleFS.open("/data.csv", "w");
  if (!out) return;
  for (size_t i = desde; i < lineas.size(); i++) {
    if (lineas[i].length() > 0) out.println(lineas[i]);
  }
  out.close();

  Serial.println("Archivo /data.csv recortado por tamano.");
}

long getLastSyncedTimestamp() {
  File f = LittleFS.open("/sync_state.txt", "r");
  if (!f) return 0;
  String s = f.readString();
  f.close();
  return s.toInt();
}

void setLastSyncedTimestamp(long ts) {
  File f = LittleFS.open("/sync_state.txt", "w");
  if (!f) return;
  f.print(ts);
  f.close();
}

// =========================================================
//              SINCRONIZACION CON THINGSPEAK
// =========================================================
void syncPendingData() {
  if (WiFi.status() != WL_CONNECTED) return; // sin WiFi, ni intentamos

  long lastSynced = getLastSyncedTimestamp();

  File f = LittleFS.open("/data.csv", "r");
  if (!f) return;

  while (f.available()) {
    String linea = f.readStringUntil('\n');
    if (linea.length() == 0) continue;

    long ts; float tDS, hum, pres;
    if (sscanf(linea.c_str(), "%ld,%f,%f,%f", &ts, &tDS, &hum, &pres) != 4) continue;

    if (ts <= lastSynced) continue; // ya sincronizado antes

    bool ok = enviarAThingSpeak(tDS, hum, pres);
    if (ok) {
      lastSynced = ts;
      setLastSyncedTimestamp(lastSynced); // guardamos avance por si se corta la luz
    } else {
      Serial.println("Fallo el envio a la nube, se reintentara en el proximo ciclo.");
      break; // cortamos: probablemente se fue el internet, no seguir insistiendo ahora
    }
    delay(1200); // ThingSpeak exige minimo ~15s entre updates reales; si hay muchas
                 // pendientes, este delay evita mandarlas demasiado rapido
  }
  f.close();
}

bool enviarAThingSpeak(float tDS, float hum, float pres) {
  HTTPClient http;
  // Orden segun el canal: Field1=Temperatura DS18B20, Field2=Humedad, Field3=Presion
  String url = "http://api.thingspeak.com/update?api_key=" + String(THINGSPEAK_API_KEY) +
               "&field1=" + String(tDS, 2) +
               "&field2=" + String(hum, 2) +
               "&field3=" + String(pres, 2);

  http.begin(url);
  int httpCode = http.GET();
  http.end();

  return (httpCode == 200);
}

// =========================================================
//                  LECTURA DE SENSORES
// =========================================================
void setupSensors() {
  dht.begin();
  ds18b20.begin();
  if (!bmp.begin(BMP280_ADDRESS)) {
    Serial.println("ERROR: BMP280 no responde.");
  }
}

Reading readSensors() {
  Reading r;
  r.valid = true;

  // El DHT22 solo se usa para humedad. No se guarda su temperatura:
  // la unica temperatura del proyecto es la del DS18B20.
  r.humedad = dht.readHumidity();
  if (isnan(r.humedad)) {
    Serial.println("  -> Fallo especifico: DHT22 no pudo leer humedad.");
    r.valid = false;
  }

  ds18b20.requestTemperatures();
  r.tempDS = ds18b20.getTempCByIndex(0);
  if (r.tempDS == DEVICE_DISCONNECTED_C) {
    Serial.println("  -> Fallo especifico: DS18B20 desconectado o no detectado.");
    r.valid = false;
  }

  // El BMP280 tambien mide temperatura internamente, pero solo usamos su presion.
  r.presion = bmp.readPressure() / 100.0F;

  r.timestamp = tiempoSincronizado() ? time(nullptr) : (time_t)(millis() / 1000);

  return r;
}

// =========================================================
//                    SERVIDOR WEB LOCAL
// =========================================================
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("Servidor web local iniciado.");
}

void handleData() {
  File f = LittleFS.open("/data.csv", "r");
  if (!f) {
    server.send(500, "application/json", "{}");
    return;
  }

  std::vector<String> lineas;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    if (l.length() > 0) lineas.push_back(l);
  }
  f.close();

  int total = lineas.size();
  int desde = max(0, total - CHART_POINTS);

  String json;
  String ts = "[", tDS = "[", hum = "[", pres = "[";

  for (int i = desde; i < total; i++) {
    long t; float a, b, c;
    if (sscanf(lineas[i].c_str(), "%ld,%f,%f,%f", &t, &a, &b, &c) != 4) continue;
    if (ts.length() > 1) { ts += ","; tDS += ","; hum += ","; pres += ","; }
    ts += String(t); tDS += String(a, 1); hum += String(b, 1); pres += String(c, 1);
  }
  ts += "]"; tDS += "]"; hum += "]"; pres += "]";

  json = "{\"ts\":" + ts + ",\"tempDS\":" + tDS + ",\"hum\":" + hum +
         ",\"pres\":" + pres + "}";

  server.send(200, "application/json", json);
}

void handleRoot() {
  server.send(200, "text/html", PAGINA_HTML);
}
