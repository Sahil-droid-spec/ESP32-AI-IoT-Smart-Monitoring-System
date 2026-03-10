#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

const char* ssid     = "yourwifiname";
const char* password = "yourwifipassword";

#define TRIG         5
#define ECHO         4
#define LED_CLOSE    23
#define LED_FAR      19
#define LED_WEB      2
#define DHTPIN       27
#define DHTTYPE      DHT11
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define HISTORY_SIZE  20
#define SMOOTH_SIZE   5

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);

bool ledState       = false;
bool blinkState     = false;
bool anomaly        = false;
bool historyFull    = false;

float distance      = 999.0;
float prevDistance  = 999.0;
float speed         = 0.0;
float eta           = 0.0;
float temperature   = 0.0;
float humidity      = 0.0;

float distBuf[SMOOTH_SIZE]    = {999,999,999,999,999};
float distHist[HISTORY_SIZE]  = {0};
float tempHist[HISTORY_SIZE]  = {0};
float humHist[HISTORY_SIZE]   = {0};

int distIdx    = 0;
int histIndex  = 0;

String zone       = "CLEAR";
String motionState = "STATIONARY";
String risk       = "SAFE";
String aiState    = "NORMAL";

unsigned long prevSensorTime = 0;
unsigned long prevBlinkTime  = 0;
unsigned long startTime      = 0;

float readRaw() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long dur = pulseIn(ECHO, HIGH, 25000);
  if (dur == 0) return 999.0;
  float d = dur * 0.034 / 2.0;
  if (d < 2.0 || d > 400.0) return 999.0;
  return d;
}

float getDistance() {
  distBuf[distIdx] = readRaw();
  distIdx = (distIdx + 1) % SMOOTH_SIZE;
  float s = 0;
  for (int i = 0; i < SMOOTH_SIZE; i++) s += distBuf[i];
  return s / SMOOTH_SIZE;
}

void detectMotion() {
  speed = prevDistance - distance;
  if (speed > 1.5)       motionState = "APPROACHING";
  else if (speed < -1.5) motionState = "RECEDING";
  else                   motionState = "STATIONARY";
  eta = (speed > 0 && distance < 400) ? distance / speed : 0;
}

void classifyDistance() {
  if      (distance < 15)  zone = "DANGER";
  else if (distance < 40)  zone = "CLOSE";
  else if (distance < 100) zone = "SAFE";
  else                     zone = "CLEAR";
}

void evaluateRisk() {
  if      (distance < 15)    risk = "PROXIMITY";
  else if (temperature > 35) risk = "HEAT";
  else if (humidity > 80)    risk = "HUMID";
  else                       risk = "SAFE";
}

void detectAnomaly() {
  anomaly  = false;
  aiState  = "NORMAL";
  if (distance < 10)       { anomaly = true; aiState = "COLLISION RISK"; }
  else if (abs(speed) > 25){ anomaly = true; aiState = speed > 0 ? "FAST APPROACH" : "FAST RECESSION"; }
  else if (temperature > 38){ anomaly = true; aiState = "HEAT SPIKE"; }
}

void updateHistory() {
  distHist[histIndex] = (distance >= 400) ? 0 : distance;
  tempHist[histIndex] = temperature;
  humHist[histIndex]  = humidity;
  histIndex = (histIndex + 1) % HISTORY_SIZE;
  if (histIndex == 0) historyFull = true;
}

String uptime() {
  unsigned long sec = (millis() - startTime) / 1000;
  char buf[12];
  sprintf(buf, "%02d:%02d:%02d", (int)(sec/3600), (int)((sec%3600)/60), (int)(sec%60));
  return String(buf);
}

String zoneColor(String z) {
  if (z == "DANGER") return "#ff2244";
  if (z == "CLOSE")  return "#ffaa00";
  if (z == "SAFE")   return "#00f5d4";
  return "#888888";
}

String riskColor(String r) {
  if (r == "PROXIMITY") return "#ff2244";
  if (r == "HEAT")      return "#ff7700";
  if (r == "HUMID")     return "#00aaff";
  return "#00f5d4";
}

void updateOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.setCursor(0, 0);
  display.println("SMART SENSOR NODE");
  display.drawFastHLine(0, 9, 128, WHITE);

  display.setCursor(0, 12);
  if (distance >= 400) {
    display.println("Dist: ---  [" + zone + "]");
  } else {
    display.print("Dist:");
    display.print(distance, 1);
    display.print(" [");
    display.print(zone);
    display.println("]");
  }

  display.setCursor(0, 22);
  display.print(motionState);
  display.print(" ");
  display.print(abs(speed), 1);
  display.println("cm/s");

  display.setCursor(0, 32);
  display.print("ETA:");
  display.print(eta > 0 ? String(eta, 1) + "s" : "--");
  display.print(" R:");
  display.println(risk);

  display.setCursor(0, 42);
  display.print("T:");
  display.print(temperature, 1);
  display.print("C H:");
  display.print(humidity, 0);
  display.println("%");

  display.drawFastHLine(0, 52, 128, WHITE);
  display.setCursor(0, 54);
  if (anomaly) {
    display.print("!");
    display.println(aiState);
  } else {
    display.print("OK ");
    display.println(uptime());
  }

  display.display();
}

void updateLEDs() {
  if (distance < 15) {
    digitalWrite(LED_FAR, LOW);
    if (millis() - prevBlinkTime >= 150) {
      blinkState = !blinkState;
      digitalWrite(LED_CLOSE, blinkState ? HIGH : LOW);
      prevBlinkTime = millis();
    }
  } else if (distance < 40) {
    digitalWrite(LED_CLOSE, HIGH);
    digitalWrite(LED_FAR, LOW);
  } else {
    digitalWrite(LED_CLOSE, LOW);
    digitalWrite(LED_FAR, HIGH);
  }
}

String buildHistJS() {
  int count = historyFull ? HISTORY_SIZE : histIndex;
  String d = "[", t = "[", h = "[";
  for (int i = 0; i < count; i++) {
    int idx = historyFull ? (histIndex + i) % HISTORY_SIZE : i;
    d += String(distHist[idx], 1);
    t += String(tempHist[idx], 1);
    h += String(humHist[idx], 1);
    if (i < count - 1) { d += ","; t += ","; h += ","; }
  }
  return "var dD=" + d + "];var tD=" + t + "];var hD=" + h + "];";
}

void handleRoot() {
  String zc = zoneColor(zone);
  String rc = riskColor(risk);
  float distPct = (distance >= 400) ? 0 : max(0.0f, min(100.0f, (1.0f - distance / 200.0f) * 100.0f));
  String distStr = (distance >= 400) ? "---" : String(distance, 1) + " cm";
  String etaStr  = (eta > 0) ? String(eta, 1) + " s" : "--";

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta http-equiv='refresh' content='2'/>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'/>";
  html += "<title>ESP32 Monitor</title><style>";
  html += "*{box-sizing:border-box;margin:0;padding:0}";
  html += "body{background:#020c14;color:#00f5d4;font-family:monospace;padding:10px}";
  html += "h1{text-align:center;font-size:1em;letter-spacing:3px;padding:10px 0;border-bottom:1px solid #00f5d4;margin-bottom:10px}";
  html += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}";
  html += ".card{background:#040f18;border:1px solid #1a3a3a;padding:10px;border-radius:6px}";
  html += ".full{grid-column:1/-1}";
  html += ".lbl{font-size:0.65em;color:#007a6a;letter-spacing:1px;margin-bottom:2px}";
  html += ".val{font-size:1.05em;font-weight:bold;margin-bottom:6px}";
  html += ".rbar{width:100%;height:20px;background:#011;border-radius:4px;overflow:hidden;margin:6px 0}";
  html += ".rfill{height:100%;border-radius:4px;transition:width 0.4s}";
  html += "canvas{width:100%;height:70px;display:block;background:#011;border-radius:4px;margin-top:4px}";
  html += ".zone{display:inline-block;padding:2px 8px;border-radius:3px;font-size:0.85em}";
  html += "a{display:inline-block;padding:7px 16px;border-radius:4px;text-decoration:none;margin:3px;font-size:0.85em}";
  html += ".on{background:#00f5d4;color:#020c14}";
  html += ".off{background:#1a3a3a;color:#00f5d4;border:1px solid #00f5d4}";
  html += ".alert{color:#ff2244;font-weight:bold}";
  html += "@keyframes blink{0%,100%{opacity:1}50%{opacity:0.2}}";
  html += ".blink{animation:blink 0.8s infinite}";
  html += "</style></head><body>";

  html += "<h1>&#9654; ESP32 SMART MONITOR</h1>";
  html += "<div class='grid'>";

  html += "<div class='card'>";
  html += "<div class='lbl'>DISTANCE</div><div class='val'>" + distStr + "</div>";
  html += "<div class='lbl'>ZONE</div><div class='val' style='color:" + zc + "'>" + zone + "</div>";
  html += "<div class='lbl'>PROXIMITY RADAR</div>";
  html += "<div class='rbar'><div class='rfill' style='width:" + String(distPct, 0) + "%;background:" + zc + "'></div></div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<div class='lbl'>MOTION</div><div class='val'>" + motionState + "</div>";
  html += "<div class='lbl'>SPEED</div><div class='val'>" + String(abs(speed), 1) + " cm/s</div>";
  html += "<div class='lbl'>ETA</div><div class='val'>" + etaStr + "</div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<div class='lbl'>TEMPERATURE</div><div class='val'>" + String(temperature, 1) + " &deg;C</div>";
  html += "<div class='lbl'>HUMIDITY</div><div class='val'>" + String(humidity, 1) + " %</div>";
  html += "</div>";

  html += "<div class='card'>";
  html += "<div class='lbl'>RISK</div><div class='val' style='color:" + rc + "'>" + risk + "</div>";
  html += "<div class='lbl'>AI STATUS</div>";
  if (anomaly)
    html += "<div class='val alert blink'>&#9888; " + aiState + "</div>";
  else
    html += "<div class='val' style='color:#00f5d4'>&#10003; NORMAL</div>";
  html += "</div>";

  html += "<div class='card full'>";
  html += "<div class='lbl'>DISTANCE HISTORY (cm)</div><canvas id='dc'></canvas>";
  html += "<div class='lbl' style='margin-top:8px'>TEMPERATURE (&deg;C) / HUMIDITY (%)</div><canvas id='tc'></canvas>";
  html += "</div>";

  html += "<div class='card full'>";
  html += "<div class='lbl'>NETWORK</div>";
  html += "<div class='val'>RSSI: " + String(WiFi.RSSI()) + " dBm &nbsp; UPTIME: " + uptime() + "</div>";
  html += "</div>";

  html += "<div class='card full' style='text-align:center'>";
  html += "<a class='on' href='/ledon'>&#9679; LED ON</a>";
  html += "<a class='off' href='/ledoff'>&#9675; LED OFF</a>";
  html += "</div>";

  html += "</div><script>";
  html += buildHistJS();
  html += "function draw(id,datasets){";
  html += "var c=document.getElementById(id);if(!c)return;";
  html += "c.width=c.offsetWidth||300;c.height=70;";
  html += "var ctx=c.getContext('2d');";
  html += "ctx.fillStyle='#011';ctx.fillRect(0,0,c.width,c.height);";
  html += "datasets.forEach(function(ds){";
  html += "if(ds.data.length<2)return;";
  html += "var mn=Math.min.apply(null,ds.data)-1;";
  html += "var mx=Math.max.apply(null,ds.data)+1;";
  html += "ctx.strokeStyle=ds.color;ctx.lineWidth=2;ctx.beginPath();";
  html += "ds.data.forEach(function(v,i){";
  html += "var x=i/(ds.data.length-1)*c.width;";
  html += "var y=c.height-(v-mn)/(mx-mn)*c.height;";
  html += "i===0?ctx.moveTo(x,y):ctx.lineTo(x,y);});ctx.stroke();});};";
  html += "draw('dc',[{data:dD,color:'#00f5d4'}]);";
  html += "draw('tc',[{data:tD,color:'#ff7700'},{data:hD,color:'#00aaff'}]);";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void ledOn() {
  ledState = true;
  digitalWrite(LED_WEB, HIGH);
  server.sendHeader("Location", "/");
  server.send(303);
}

void ledOff() {
  ledState = false;
  digitalWrite(LED_WEB, LOW);
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(LED_CLOSE, OUTPUT);
  pinMode(LED_FAR, OUTPUT);
  pinMode(LED_WEB, OUTPUT);

  digitalWrite(LED_CLOSE, LOW);
  digitalWrite(LED_FAR, LOW);
  digitalWrite(LED_WEB, LOW);

  dht.begin();
  Wire.begin(26, 25);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED FAILED");
    while (true) delay(1000);
  }

  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.begin(ssid, password);
  unsigned long wt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wt > 15000) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("WiFi FAILED!");
      display.println("Check credentials");
      display.display();
      while (true) delay(1000);
    }
    delay(500);
  }

  digitalWrite(LED_WEB, HIGH);

  String ip = WiFi.localIP().toString();
  Serial.println("IP: " + ip);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WiFi Connected!");
  display.setCursor(0, 16);
  display.println(ip);
  display.setCursor(0, 32);
  display.println("Open browser:");
  display.setCursor(0, 42);
  display.println("http://" + ip);
  display.display();
  delay(3000);

  server.on("/", handleRoot);
  server.on("/ledon", ledOn);
  server.on("/ledoff", ledOff);
  server.begin();

  startTime = millis();
}

void loop() {
  server.handleClient();
  updateLEDs();

  if (millis() - prevSensorTime >= 2000) {
    prevDistance = distance;
    distance     = getDistance();

    detectMotion();
    classifyDistance();
    detectAnomaly();

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) temperature = t;
    if (!isnan(h)) humidity    = h;

    evaluateRisk();
    updateHistory();
    updateOLED();

    prevSensorTime = millis();
  }
}
