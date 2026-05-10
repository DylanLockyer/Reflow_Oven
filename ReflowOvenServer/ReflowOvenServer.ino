#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <Arduino_JSON.h>
#include "LittleFS.h"
#include <SPI.h>
#include "Adafruit_MAX31855.h"


// Thermocouple pins
#define MAXDO   12
#define MAXCS   4
#define MAXCLK  14

#define SSR 5


// initialize the Thermocouple
Adafruit_MAX31855 thermocouple(MAXCLK, MAXCS, MAXDO);


const String SSID = "Reflow Oven";
const byte DNS_PORT = 53;



IPAddress apIP(172, 217, 28, 1);
DNSServer dnsServer;
ESP8266WebServer webServer(80);

String responseHTML = "";
double temperature = 0;
bool startStop = false;
bool isOn = false;
JSONVar profile;

long halfTime = 0;
long startTime = 0;

void setup() {

  // Serial initialisation
  Serial.begin(74880);
  Serial.println("hello");
  // Remove
  if (!LittleFS.begin()) {
  Serial.println("LittleFS mount FAILED");
  } else {
    Serial.println("LittleFS mounted OK");
    Dir root = LittleFS.openDir("/");
    while (root.next()) {
      Serial.println(root.fileName());
    }
  }
  // remove end

  //SSR initialization
  pinMode(SSR, OUTPUT);

  //Initialize wifi
  responseHTML = openReflowCode();
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(SSID);  https://randomnerdtutorials.com/

  // if DNSServer is started with "*" for domain name, it will reply with
  // provided IP to all DNS request
  dnsServer.start(DNS_PORT, "*", apIP);


  // replay to all requests with same HTML
  webServer.onNotFound([]() {
    webServer.send(200, "text/html", responseHTML);
  });


  // Interupt to send current temperature on request
  webServer.on("/temperature", []() {
    webServer.send(200, "text/plain", String(temperature));
  });


  // Command to start/stop reflow
  webServer.on("/stopStart", HTTP_POST, []() {
    String arg = webServer.arg("plain");
    if (arg == "STOP") {
      startStop = false;
    } else if (arg == "START") {
      startStop = true;
      startTime = millis();
    }
    webServer.send(200, "text/plain", "OK");
  });


  // Interupt for recieving the reflow profile for PID loop targets
  webServer.on("/updateProfile", HTTP_POST, []() {
    profile = JSON.parse(webServer.arg("plain"));
    webServer.send(200, "text/plain", "OK");
  });


  // Interupt for saving new reflow profile
  webServer.on("/saveProfile", HTTP_POST, []() {
    JSONVar saveProfile = JSON.parse(webServer.arg("plain"));
    String verification = writeProfile(saveProfile["name"], saveProfile);
    webServer.send(200, "application/json", "OK");
  });


  // Interupt for returning list of all available profiles
  webServer.on("/getProfiles", HTTP_POST, []() {
    String profileNames = readProfileNames();
    webServer.send(200, "text/plain", profileNames);
  });


  // Interupt for sending the content of a specific profile
  webServer.on("/readProfile", HTTP_POST, []() {
    String profileSend = webServer.arg("plain");
    String profileContents = readProfileContent(profileSend);
    webServer.send(200, "text/plain", profileContents);
  });

  // Create the webserver
  webServer.begin();
}


// Main loop
void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();
  getTemperature();

  if (startStop == true) {
  acPowerCalculate(pidLoop());
  } 
  else {
    acPowerCalculate(0);
  } 
}


// Get temperature from probe (not yet implemented)
void getTemperature() {
  temperature = thermocouple.readCelsius();
}


// PID loop return value from 0-100 to represent power
int pidLoop() {
  int currentTime = millis() - startTime;
  int requiredTemp = 0;
  for(int i = 0; i < profile["x"].length(); i++){
    Serial.println(currentTime <= (int)profile["x"][i]);
    if (currentTime <= (int)profile["x"][i]) { 
      int lowTime = profile["x"][i - 1];
      int highTime = profile["x"][i];
      int lowTemp = profile["y"][i - 1];
      int highTemp = profile["y"][i];

      int tempSlope = (highTemp - lowTemp) / (highTime - lowTime);
      requiredTemp = lowTemp + tempSlope * (currentTime - lowTime);
      break;
    }
  }
  // Serial.println(requiredTemp);

  return 100;
}


// Calculate ac period on and off then write to SSR
void acPowerCalculate(float power) {
  if (power < 0) {power = 0;}
  if (power > 100) {power = 100;}

  float onTimeTemp = map(power, 0, 100, 0, 30);
  int onTime = round(onTimeTemp / 2) * 2;

  if (millis() >= ((halfTime + 500))) { halfTime += 500; }

  if ((millis() - halfTime) < (((float)onTime / 60) * 1000)) {
    digitalWrite(SSR, HIGH);
  }
  else {
    digitalWrite(SSR, LOW);
  }
}


// Fetches the HTML script from memory
String openReflowCode() {
  if (!LittleFS.begin()){
    return "null";
  }
  File htmlFile = LittleFS.open("/reflow_code.html", "r");
  if(!htmlFile){  
    return "null";
  }
  String fileContents = htmlFile.readString();    

  htmlFile.close();
  return fileContents;
}


// Get a list of all available profiles
String readProfileNames() {
  Dir profiles = LittleFS.openDir("profile");
  JSONVar profileNames;
  int i = 0;
  while(profiles.next()) {
    profileNames[i] = profiles.fileName();
    i++;
  }
  return JSON.stringify(profileNames);
}


// Read the reflow x/y values from text
String readProfileContent(String name) {
  File profileFile = LittleFS.open("/profile/" + name + ".json", "r");
  if (!profileFile.isFile()){
    return "none";
  } else {
    String readProfile = profileFile.readString();
    profileFile.close();
    return readProfile;
  }
  
}


// Save a new solder reflow profile
String writeProfile(String name, JSONVar content) {

  if (!LittleFS.openDir("profile").isDirectory()) {
    LittleFS.mkdir("profile");
  }
  
  File newFile = LittleFS.open("/profile/" + name + ".json", "w");
  newFile.println(content);
  delay(100);
  newFile.close();
  return "OK";
}