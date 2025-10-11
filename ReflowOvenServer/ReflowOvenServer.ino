#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <Arduino_JSON.h>
#include "LittleFS.h"


const String SSID = "Reflow Oven";
const byte DNS_PORT = 53;


IPAddress apIP(172, 217, 28, 1);
DNSServer dnsServer;
ESP8266WebServer webServer(80);

String responseHTML = "";
int temperature = 0;
bool startStop = false;
JSONVar profile;

void setup() {

  // Serial initialisation
  Serial.begin(74880);


  // Initialize wifi
  responseHTML = openReflowCode();
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(SSID);https://randomnerdtutorials.com/

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
    }
    Serial.println(startStop);
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
    Serial.println("saveProfile Verification " + verification);
    webServer.send(200, "application/json", "OK");
  });


  // Interupt for returning list of all available profiles
  webServer.on("/getProfiles", HTTP_POST, []() {
    String profileNames = readProfileNames();
    webServer.send(200, "text/plain", profileNames);
    Serial.println(profileNames);
  });


  // Interupt for sending the content of a specific profile
  webServer.on("/readProfile", HTTP_POST, []() {
    String profile = webServer.arg("plain");
    String profileContents = readProfileContent(profile);
    webServer.send(200, "text/plain", profileContents);
    Serial.println(profileContents);
  });

  // Create the webserver
  webServer.begin();
}


// Main loop
void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();
  temperature++;
  run();
}


// Fetches the HTML script from memory
String openReflowCode() {
  if (!LittleFS.begin()){
    Serial.println("LittleFs cannot start");
    return "null";
  }
  File htmlFile = LittleFS.open("/reflow_code.html", "r");
  if(!htmlFile){  
    Serial.println("File doesn't exist");
    return "null";
  }
  String fileContents = htmlFile.readString();    

  htmlFile.close();
  return fileContents;
}


// Function for handling temperature control
void run(){
  if (startStop == true) {

    //Placeholder actual function to do reflow will be required
    //Serial.println("running");
  }
}


// Get a list of all available profiles
String readProfileNames() {
  Dir profile = LittleFS.openDir("profile");
  JSONVar profileNames;
  int i = 0;
  while(profile.next()) {
    profileNames[i] = profile.fileName();
    i++;
  }
  Serial.println("got list of names");
  return JSON.stringify(profileNames);
}


// Read the reflow x/y values from text
String readProfileContent(String name) {
  Serial.println("starting to read profile " + name);
  File profileFile = LittleFS.open("/profile/" + name + ".json", "r");
  if (!profileFile.isFile()){
    Serial.println("profile doesn't exist");
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
  Serial.println("saved profile");
  return "OK";
}