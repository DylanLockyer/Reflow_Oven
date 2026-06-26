#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <Arduino_JSON.h>
#include "LittleFS.h"
#include <SPI.h>
#include "Adafruit_MAX31855.h"

// Holds pid data in this order
// KP, KI, KD, pValue, iValue, dValue, setpoint, error, power, running
// Contains default PID values, updated through GUI
double PID[10] = {1.0, 0.05, 1.0};

#define I_MAX 100

double last_error = 0, i_term = 0;
unsigned long nextOnTime = 0, nextOffTime = 0;

// Thermocouple pins
#define MAXDO   12
#define MAXCS   4
#define MAXCLK  14

#define SSR 5

#define pidFrequency 100  // ms between PID updates

int tempIncrement = 0;
double setpoint = 0;    // global so it persists across pidLoop() calls
int prevPower = 0;

// Initialize the thermocouple
Adafruit_MAX31855 thermocouple(MAXCLK, MAXCS, MAXDO);

const String SSID = "Reflow Oven";
const byte DNS_PORT = 53;

IPAddress apIP(172, 217, 28, 1);
DNSServer dnsServer;
ESP8266WebServer webServer(80);

String responseHTML = "";
double temperature = 0;
bool startStop = false;
JSONVar profile;

long prevTime = 0;
unsigned long reflowStartTime = 0;


// Ring buffer stuff
#define BUFFER_SIZE 20
double buffer[BUFFER_SIZE] = {0};
int bufferHead = 0;
int bufferCount = 0;

unsigned long prevTempTime = 0;
#define tempReadFreq 100 // Read frequency in ms

// ─── Setup ──────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(74880);
  Serial.println("hello");

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount FAILED");
  } else {
    Serial.println("LittleFS mounted OK");
    loadPIDFromFile();
    Dir root = LittleFS.openDir("/");
    while (root.next()) {
      Serial.println(root.fileName());
    }
  }

  pinMode(SSR, OUTPUT);

  responseHTML = openReflowCode();
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(SSID);

  dnsServer.start(DNS_PORT, "*", apIP);

  webServer.onNotFound([]() {
    webServer.send(200, "text/html", responseHTML);
  });

  // Current temperature
  webServer.on("/temperature", []() {
    webServer.send(200, "text/plain", String(temperature));
  });

  // Start / stop reflow
  webServer.on("/stopStart", HTTP_POST, []() {
    String arg = webServer.arg("plain");
    if (arg == "STOP") {
      startStop = false;
      PID[9] = 0;
    } else if (arg == "START") {
      startStop = true;
      PID[9] = 1;
      reflowStartTime = millis();
      prevTime = millis();
      tempIncrement = 0;
      setpoint = temperature;   // start from current temp
      profile["y"][0] = temperature;
      last_error = 0;
      i_term = 0;
    }
    webServer.send(200, "text/plain", "OK");
  });

  // Receive profile for active PID loop
  webServer.on("/updateProfile", HTTP_POST, []() {
    profile = JSON.parse(webServer.arg("plain"));
    webServer.send(200, "text/plain", "OK");
  });

  // Save profile to flash
  webServer.on("/saveProfile", HTTP_POST, []() {
    JSONVar saveProfile = JSON.parse(webServer.arg("plain"));
    writeProfile("profile", saveProfile["name"], saveProfile);
    webServer.send(200, "application/json", "OK");
  });

  // Delete profile from flash
  webServer.on("/removeProfile", HTTP_POST, []() {
    JSONVar req = JSON.parse(webServer.arg("plain"));
    removeProfile("profile", req["name"]);
    webServer.send(200, "text/plain", "OK");
  });

  // List all profiles
  webServer.on("/getProfiles", HTTP_POST, []() {
    webServer.send(200, "text/plain", readProfileNames());
  });

  // Read a specific profile
  webServer.on("/readProfile", HTTP_POST, []() {
    String profileContents = readProfileContent("profile", webServer.arg("plain"));
    webServer.send(200, "text/plain", profileContents);
  });

  // Update PID tuning values at runtime
  webServer.on("/updatePID", HTTP_POST, []() {
    JSONVar pid = JSON.parse(webServer.arg("plain"));
    if (pid.hasOwnProperty("kp")) PID[0] = (double)pid["kp"];
    if (pid.hasOwnProperty("ki")) PID[1] = (double)pid["ki"];
    if (pid.hasOwnProperty("kd")) PID[2] = (double)pid["kd"];
    i_term = 0;
    last_error = 0;
    savePIDToFile();
    webServer.send(200, "text/plain", "OK");
  });

  webServer.on("/getPID", HTTP_GET, []() {
    JSONVar pidJson;
    pidJson["kp"]       = PID[0];
    pidJson["ki"]       = PID[1];
    pidJson["kd"]       = PID[2];
    pidJson["pValue"]   = PID[3];
    pidJson["iValue"]   = PID[4];
    pidJson["dValue"]   = PID[5];
    pidJson["setpoint"] = PID[6];
    pidJson["error"]    = PID[7];
    pidJson["power"]    = PID[8];
    pidJson["running"] = PID[9];
    webServer.send(200, "application/json", JSON.stringify(pidJson));
  });

  webServer.begin();
}


void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();
  getTemperature();

  if (startStop) {
    acPowerCalculate(pidLoop());
  } else {
    acPowerCalculate(0);
    tempIncrement = 0;
  }
}


void getTemperature() {
  if (millis() > prevTempTime + tempReadFreq){
    double tempRead = thermocouple.readCelsius(); 
    if (!isnan(tempRead)){
      bufferAppend(tempRead);
    }
    temperature = bufferAverage();
    prevTempTime = millis();
  }
}

// ─── Target temperature interpolation ────────────────────────────────────────

// Returns degrees-per-millisecond ramp rate for the current segment,
// or 0 if we are at/past the final waypoint.
double targetTempRate() {
  int len = profile["x"].length();
  if (tempIncrement < len - 1) {
    double tempDiff = (double)profile["y"][tempIncrement + 1] - (double)profile["y"][tempIncrement];
    double timeDiff = (double)profile["x"][tempIncrement + 1] - (double)profile["x"][tempIncrement];
    if (timeDiff <= 0) return 0.0;
    // timeDiff is in seconds in the profile; pidFrequency is in ms
    return tempDiff / (timeDiff * 1000.0);  // °C per ms

  } else return 0;


}


// ─── PID calculation ─────────────────────────────────────────────────────────

int calculate_pid(double sp, double current) {
  // Get error
  double error = sp - current;

  // Get p_term
  double p_term = PID[0] * error;

  // Get I_term
  i_term += PID[1] * error;
  if (i_term >  I_MAX) i_term =  I_MAX;

  // D-term on error delta (not setpoint delta)
  double d_term = PID[2] * (error - last_error);
  last_error = error;

  // Clamp outputs to positive number
  if (p_term < 0) p_term = 0;
  if (i_term < 0) i_term = 0;
  if (d_term < 0) d_term = 0;
  
  int power = (int)(p_term + i_term + d_term);
  if (error < -5) power = 0;

  PID[3] = p_term;
  PID[4] = i_term;
  PID[5] = d_term;
  PID[6] = sp;
  PID[7] = error;
  PID[8] = power;
  // Combine outputs
  return power;
}


// ─── PID loop ────────────────────────────────────────────────────────────────

int pidLoop() {

  // Only update every pidFrequency
  long now = millis();
  if (now < prevTime + pidFrequency) return prevPower;  // not time yet
  long elapsed = now - prevTime;
  prevTime = now;

  // Advance waypoint if we have passed the next one's temperature
  int len = profile["x"].length();
  
  unsigned long totalElapsedMs = millis() - reflowStartTime;

  if (tempIncrement < len - 1) {
    if (temperature >= (double)profile["y"][tempIncrement + 1] && totalElapsedMs >= (unsigned long)((double)profile["x"][tempIncrement + 1] * 1000.0)) {
      tempIncrement++;
    }
  } else {
    startStop = false;
    PID[9] = 0;
  }
  // If reflow takes more than 1000 seconds then stop
  if (totalElapsedMs > 1000000){
    startStop = false;
    PID[9] = 0;
  }

  // Ramp setpoint forward
  double rate = targetTempRate();
  setpoint += rate * elapsed;

  // Clamp setpoint to the target waypoint ceiling so it never runs away
  if (tempIncrement < len + 1) {
    double ceiling = (double)profile["y"][tempIncrement + 1];
    if (setpoint > ceiling) setpoint = ceiling;
  }
  prevPower = calculate_pid(setpoint, temperature);
  return prevPower;
}


// ─── SSR phase-angle / burst control ─────────────────────────────────────────

void acPowerCalculate(float power) {
  // Convert power into 4 steps
  long onTime;
  if (power > 75) onTime = 1000;
  else if (power > 50) onTime = 750;
  else if (power > 25) onTime = 500;
  else if (power > 1) onTime = 250;
  else onTime = 0;
  
  // Turn ssr on for specific amount of time
  unsigned long now = millis();
  if (onTime == 1000) digitalWrite(SSR, HIGH);
  else if (onTime == 0) digitalWrite(SSR, LOW);
  else if (now >= nextOnTime) {
    digitalWrite(SSR, HIGH);
    nextOffTime = now + onTime;
    nextOnTime = now + 1000;
  }
  else if (now >= nextOffTime) digitalWrite(SSR, LOW);

}


// ─── LittleFS helpers ────────────────────────────────────────────────────────

String openReflowCode() {
  if (!LittleFS.begin()) return "null";
  File htmlFile = LittleFS.open("/reflow_code.html", "r");
  if (!htmlFile) return "null";
  String fileContents = htmlFile.readString();
  htmlFile.close();
  return fileContents;
}

String readProfileNames() {
  Dir profiles = LittleFS.openDir("profile");
  JSONVar profileNames;
  int i = 0;
  while (profiles.next()) {
    profileNames[i++] = profiles.fileName();
  }
  return JSON.stringify(profileNames);
}

String readProfileContent(String directory, String name) {
  File profileFile = LittleFS.open("/" + directory + "/" + name + ".json", "r");
  if (!profileFile.isFile()) return "none";
  String readProfile = profileFile.readString();
  profileFile.close();
  return readProfile;
}
// profile
void writeProfile(String directory, String name, JSONVar content) {
  if (!LittleFS.openDir("profile").isDirectory()) {
    LittleFS.mkdir("profile");
  }
  File newFile = LittleFS.open("/" + directory + "/" + name + ".json", "w");
  newFile.println(content);
  delay(100);
  newFile.close();
}

void removeProfile(String directory, String name) {
  LittleFS.remove("/" + directory + "/" + name + ".json");
}

void savePIDToFile() {
  JSONVar pidJson;
  pidJson["kp"] = PID[0];
  pidJson["ki"] = PID[1];
  pidJson["kd"] = PID[2];
  File f = LittleFS.open("/pidConfig.json", "w");
  if (f) {
    f.print(JSON.stringify(pidJson));
    f.close();
  }
}

void loadPIDFromFile() {
  File f = LittleFS.open("/pidConfig.json", "r");
  if (!f) return;  // no saved config yet — keep defaults
  String content = f.readString();
  f.close();
  JSONVar pidJson = JSON.parse(content);
  if (JSON.typeof(pidJson) == "undefined") return;
  if (pidJson.hasOwnProperty("kp")) PID[0] = (double)pidJson["kp"];
  if (pidJson.hasOwnProperty("ki")) PID[1] = (double)pidJson["ki"];
  if (pidJson.hasOwnProperty("kd")) PID[2] = (double)pidJson["kd"];
}

// ─── Ring Buffer Implementation ────────────────────────────────────────────────────────

// Add new value to buffer
void bufferAppend(double temperature) {
  buffer[bufferHead] = temperature;
  bufferHead = (bufferHead + 1) % BUFFER_SIZE;
  if (bufferCount < BUFFER_SIZE) {
    bufferCount++;
  }
}

// Read specific values from buffer
double bufferRead(int position) {
  if (position < 0 || position >= bufferCount) {
    return 0.0; // or handle error as needed
  }
  int index = (bufferHead - 1 - position + BUFFER_SIZE) % BUFFER_SIZE;
  return buffer[index];
}

//Get average of all real data in buffer
double bufferAverage() {
  if (bufferCount == 0) {
    return 0.0;
  }
  double sum = 0.0;
  for (int i = 0; i < bufferCount; i++) {
    sum += buffer[i];
  }
  return sum / bufferCount;
}
