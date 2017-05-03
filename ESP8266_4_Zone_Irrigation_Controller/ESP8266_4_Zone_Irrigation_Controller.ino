/**
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 *  in compliance with the License. You may obtain a copy of the License at:
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software distributed under the License is distributed
 *  on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License
 *  for the specific language governing permissions and limitations under the License.
 *
 * A Basic ESP8266 Device to Integrate with Samsung Smart Things
 * 
 *
 * Author: Jacob Schreiver 
 * Date: 2016-02-21
 *
 */

#include <FS.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>

#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <ESP8266SSDP.h>
//#include <Event.h>
#include <Timer.h>



//const int ledZone1 = 13;    // Pin labeled GPIO13

const char* version = "2.0.6";

//user configurable global variables to set before loading to Arduino
int maxrelays = 16;  //set up before loading to Arduino (maximum possible relays)
int relays = 4;  //set up before loading to Arduino (max = 8 with current code)
boolean isActiveHigh=true; //set to true if using "active high" relay, set to false if using "active low" relay
boolean isDebugEnabled=false;    // enable or disable debug in this example
boolean isPin4Pump=false;  //set to true if you add an additional relay to pin4 and use as pump or master valve.  

//set global variables


const char APPSETTINGS[] PROGMEM = "/appSettings.json";
const char LOADED[] PROGMEM = " loaded: ";
const char HUBPORT[] PROGMEM = "hubPort";
const char HUPIP[] PROGMEM = "hubIp";
const char DEVICENAME[] PROGMEM = "deviceName";


// Smartthings hub information
IPAddress hubIp = INADDR_NONE; // smartthings hub ip
unsigned int hubPort = 0; // smartthings hub port
String deviceName = "ESP8266 Irrigation Controller";




//OTA

const char* serverIndex = "<form method='POST' action='/updateOTA' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";

//Irrigation
Timer t;
int trafficCop =0;  //tracks which station has the right of way (is on)
int stations=relays; //sets number of stations = number of relays. This is software configurable in device type
int relayOn = HIGH;
int relayOff = LOW;
// initialize irrigation zone variables; for readability, zone values store [1]-[8] and [0] is not used
long stationTime[] = {0,0,0,0,0,0,0,0,0};
int8_t stationTimer[] = {0,0,0,0,0,0,0,0,0}; 
int queue[]={0,0,0,0,0,0,0,0,0};  // off: 0, queued: 1, running: 2
int relay[9];



//initialize pump related variables. 
int pin4Relay = 16;  //pin16 reserved for optional additional relay to control a pump or master valve
boolean isConfigPump = false; // if true, relay8 is used as the pump/master valve switch.  Can be toggled by device tye v2.7 and later
const char* configPumpStatus = "off";
const char* pin4PumpStatus= "off";
boolean doUpdate = false;
boolean doPumpUpdate = false;
long pumpOffDelay = 0; // set number of minutes to delay turning pump or master valve off after last zone has finished running


ESP8266WebServer server(80);
WiFiClient client; //client


int state = 1;

IPAddress IPfromString(String address) {
  int ip1, ip2, ip3, ip4;
  ip1 = address.toInt();
  address = address.substring(address.indexOf('.') + 1);
  ip2 = address.toInt();
  address = address.substring(address.indexOf('.') + 1);
  ip3 = address.toInt();
  address = address.substring(address.indexOf('.') + 1);
  ip4 = address.toInt();
  return IPAddress(ip1, ip2, ip3, ip4);
}

void handleRoot() {
  String updateStatus = makeUpdate();  
  server.send(200, "application/json", updateStatus);  
}

void handleCommand() {
  String message = server.arg("command");
  messageCallout(message);
  String updateStatus = makeUpdate();
  server.send(200, "application/json", updateStatus);
}

void handleStatus() {
  String updateStatus = makeUpdate();
  server.send(200, "application/json", updateStatus);
}

void handleReboot() {
  messageCallout("allOff");
  String updateStatus = makeUpdate();
  server.send(200, "application/json", updateStatus);
  ESP.restart();
}

void handleConfig() {
  StaticJsonBuffer<100> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  
  //If we have received an ST IP then update as needed
  if (server.hasArg("hubIp")) {
      json[FPSTR(HUPIP)] = server.arg("hubIp");
      IPAddress newHubIp = IPfromString(json[FPSTR(HUPIP)]);
      if (newHubIp != hubIp) {
        hubIp = newHubIp;
      }
  }

  //If we have received an ST Port then update as needed
  if (server.hasArg("hubPort")) {
      
      unsigned int newHubPort = atoi(server.arg("hubPort").c_str());
      json[FPSTR(HUBPORT)] = newHubPort;
      if (newHubPort != hubPort) { 
        hubPort = newHubPort;
      }
  }

  //save config
  String settingsJSON;
  json.printTo(settingsJSON);
  if (isDebugEnabled) {
    Serial.print("Saving JSON ");
    Serial.println("settingsJSON");
  }
  saveAppConfig(settingsJSON);
  
  String updateStatus = makeUpdate();
  server.send(200, "application/json", updateStatus);
}


void handleNotFound(){
  server.send(404, "text/html", "<html><body>Error! Page Not Found!</body></html>");
}

void setup(void){

//debug stuff
  if (isDebugEnabled) {
    // setup debug serial port
    Serial.begin(115200);         // setup serial with a baud rate of 9600
    Serial.println("setup..");  // print out 'setup..' on start
    Serial.print("Version:  ");
    Serial.println(version);
    Serial.print(F("Sketch size: "));
    Serial.println(ESP.getSketchSize());
    Serial.print(F("Free size: "));
    Serial.print(ESP.getFreeSketchSpace());
  }

  if (isDebugEnabled) {
    Serial.println(F("Mounting FS..."));
  }
  if (!SPIFFS.begin()) {
    if (isDebugEnabled) {
      Serial.println(F("Failed to mount file system"));
    }
    return;
  }

  // DEBUG: remove all files from file system
//  SPIFFS.format();

  if (!loadAppConfig()) {
    if (isDebugEnabled) {
      Serial.println(F("Failed to load application config"));
    }
  } else {
    if (isDebugEnabled) {
      Serial.println(F("Application config loaded"));
    }
  }

    //WiFiManager
    //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
    //reset saved settings
    //wifiManager.resetSettings();
    
    //set custom ip for portal
    //wifiManager.setAPConfig(IPAddress(10,0,1,1), IPAddress(10,0,1,1), IPAddress(255,255,255,0));

    //fetches ssid and pass from eeprom and tries to connect
    //if it does not connect it starts an access point with the specified name
    //here  "AutoConnectAP"
    //and goes into a blocking loop awaiting configuration
  wifiManager.autoConnect("AutoConnectAP");
    //or use this for auto generated name ESP + ChipID
    //wifiManager.autoConnect();

    
    //if you get here you have connected to the WiFi
  if (isDebugEnabled) {
    Serial.println("connected...yeey :)");
    Serial.println("local ip");
    Serial.println(WiFi.localIP());
  }
      
  

  // ***irrigation setup
  
  // setup timed actions 
  t.every(10 * 60L * 1000L, timeToUpdate); //send update to smartThings hub every 10 min
  t.every(1 * 60L * 1000L, queueManager);// double check queue to see if there is work to do
  
 
  
  //set up relays to control irrigation valves
  if (!isActiveHigh) {
    relayOn=LOW;
    relayOff=HIGH;
  }

   relay[1]=14;
   relay[2]=13;
   relay[3]=16;
   relay[4]=12;
  
  int i=1;
  while (i<5) {
    //relay[i]= 13-i;
    pinMode(relay[i], OUTPUT); 
    digitalWrite(relay[i], relayOff);
    if (isDebugEnabled) {
      Serial.print("Turning off Relay ");
      Serial.println(relay[i]);
    }
    i++;
  }
 


  // set up optional hardware configured pump to pin4 on Arduino
  if (isPin4Pump) {
    pinMode(pin4Relay, OUTPUT);
    digitalWrite(pin4Relay, relayOff);
    if (isDebugEnabled) {
      Serial.println("Turning off Pump");
    }
  }
  
  server.on("/", handleRoot);
  
  server.on("/command", handleCommand);
  server.on("/status", handleStatus);
  server.on("/update", HTTP_GET, [](){
      server.sendHeader("Connection", "close");
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.send(200, "text/html", serverIndex);
    });
  server.on("/updateOTA", HTTP_POST, [](){
      server.sendHeader("Connection", "close");
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.send(200, "text/plain", (Update.hasError())?"FAIL":"OK");
      ESP.restart();
    },[](){
      HTTPUpload& upload = server.upload();
      if(upload.status == UPLOAD_FILE_START){
        Serial.setDebugOutput(true);
        WiFiUDP::stopAll();
        if (isDebugEnabled) {
          Serial.printf("Update: %s\n", upload.filename.c_str());
        }
        uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if(!Update.begin(maxSketchSpace)){//start with max available size
          if (isDebugEnabled) {
            Update.printError(Serial);
          }
        }
      } else if(upload.status == UPLOAD_FILE_WRITE){
        if(Update.write(upload.buf, upload.currentSize) != upload.currentSize){
          if (isDebugEnabled) {
            Update.printError(Serial);
          }
        }
      } else if(upload.status == UPLOAD_FILE_END){
        if(Update.end(true)){ //true to set the size to the current progress
          if (isDebugEnabled) {
            Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
          }
        } else {
          if (isDebugEnabled) {
            Update.printError(Serial);
          }
        }
          if (isDebugEnabled) {
            Serial.setDebugOutput(false);
          }
      }
      yield();
    });

  server.on("/esp8266ic.xml", HTTP_GET, [](){
    if (isDebugEnabled) {
      Serial.println("Request for /esp8266ic.xml");
    }
    SSDP.schema(server.client());
  });
  server.on("/config", handleConfig);
  server.on("/reboot", handleReboot);
  server.onNotFound(handleNotFound);
  

  server.begin();

  SSDP.setSchemaURL("esp8266ic.xml");
  SSDP.setHTTPPort(80);
  SSDP.setName("ESP8266 Irrigation Controller");
  SSDP.setSerialNumber("0112358132134");
  SSDP.setURL("index.html");
  SSDP.setModelName("ESP8266Irrigation Controller");
  SSDP.setModelNumber("1");
  SSDP.setModelURL("https://github.com/anienhuis/smart_sprinkler/");
  SSDP.setManufacturer("Aaron Nienhuis");
  SSDP.setManufacturerURL("https://github.com/anienhuis/smart_sprinkler/");
    
  SSDP.begin();
  
}

void loop(void){

  if (isDebugEnabled) {
    //Serial.println("Starting loop");
  }
    //run timer 
  t.update();
  
  if (doUpdate) {
    sendUpdate("");
    doUpdate = false;
  }
  if (doPumpUpdate) {
    sendPumpUpdate();
    doPumpUpdate = false;
  }
 
  server.handleClient();
  delay(10);
  if (isDebugEnabled) {
    //Serial.println("Ending loop");
  }
}

//process incoming messages from SmartThings hub
void messageCallout(String message) 
{
  if (isDebugEnabled) {
    Serial.print("Received message: '");
    Serial.print(message);
    Serial.println("' "); 
  }
  char* inValue[maxrelays+2]; //array holds any values being delivered with message (1-8) and NULL; [0] is not used
  char delimiters[] = ",";
  char charMessage[100];
  strncpy(charMessage, message.c_str(), sizeof(charMessage));
  charMessage[sizeof(charMessage) - 1] = '\0';
  inValue[0] = strtok(charMessage,delimiters);  //remove first substring as messageType
  int i=1;
  while(inValue [i-1] != NULL) {
    inValue [i] = strtok(NULL, delimiters); //remove remaining substrings as incoming values
    i++;
  }
  if (strcmp(inValue[0],"on")==0)  {   // add new station to queue
    if (strcmp(inValue[2],"0")!=0 && strcmp(inValue[2],"null")!=0) {  //do not add to queue if zero time
      int addStation=atoi(inValue[1]);
      queue[addStation]=1;
      stationTime[addStation]=atol(inValue[2])*60L*1000L;
    }
    scheduleUpdate();
  }
  if (strcmp(inValue[0],"off")==0) {
    int removeStation=atoi(inValue[1]);     
    if (removeStation==trafficCop)  {
      toggleOff();
    }
    //else remove from queue and update queue status to SmartThings
    queue[removeStation]=0;
    scheduleUpdate();
  }
  
  if (strcmp(inValue[0],"allOn")==0) {
    if (isDebugEnabled) {
      Serial.println("Calling allOn");
    }
    int i=1;
    while (i<stations+1) {
      
      if (i != trafficCop) {
        if (strcmp(inValue[i],"0")!=0 && strcmp(inValue[i],"null")!=0) {  //do not add to queue if zero time
          queue[i]=1;
        }
      }
      stationTime[i]=atoi(inValue[i])*60L*1000L;
      i++;
    }
    scheduleUpdate();
  }
  if (strcmp(inValue[0],"allOff")==0) {
    allOff();
  }
  if (strcmp(inValue[0],"advance")==0) {
    toggleOff();
  }
  if (strcmp(inValue[0],"pump")==0) {
    if (strcmp(inValue[1],"0")==0) {
      isConfigPump = false;
//      stations = relays;
      if (stations == 7) {  
        stations = 8;
      }
    }
    if (strcmp(inValue[1],"1")==0) {
      pumpOff();
    }
    if (strcmp(inValue[1],"2")==0) {
       pumpOn();
    }
    if (strcmp(inValue[1],"3")==0) {
      isConfigPump = true;
      digitalWrite(relay[8], relayOff); 
      if (isDebugEnabled) {
        Serial.print("Turning off relay ");
        Serial.println(relay[8]);
      }
      configPumpStatus = "enabled";
//      stations = relays - 1;
      if (stations == 8) {  
        stations = 7;
      }
    }
    schedulePumpUpdate();
  }
  queueManager();
}

void scheduleUpdate() {
  if (isDebugEnabled) {
    Serial.println("Starting scheduleUpdate");
  }
  doUpdate = true; 
  if (isDebugEnabled) {
    Serial.println("Ending scheduleUpdate");
  }
}
void schedulePumpUpdate() {
  if (isDebugEnabled) {
    Serial.println("Starting schedulePumpUpdate");
  }  
  doPumpUpdate = true;
  if (isDebugEnabled) {
    Serial.println("Ending schedulePumpUpdate");
  }
}
//run through queue to check to see if there is work to do
void queueManager() {
  if (isDebugEnabled) {
    Serial.println("Starting queueManager");
  }
  
  int i=1;
  while (i<9) {
    if (trafficCop==0 && queue[i]==1) {
      //ready for next in line 
      trafficCop=i;
      toggleOn();
    }
    i++;
  }
  if (isDebugEnabled) {
    Serial.println("Ending queueManager");
  }
}

void toggleOn() {
  if (isDebugEnabled) {
    Serial.println("Starting toggleOn");
  }
  queue[trafficCop]=2;
  
  if (isConfigPump) {
    digitalWrite(relay[8], relayOn);
    if (isDebugEnabled) {
      Serial.print("Turn on Relay ");
      Serial.println(relay[8]);
    }
    configPumpStatus = "on";
  }
  if (isPin4Pump) {
    digitalWrite(pin4Relay, relayOn);
    if (isDebugEnabled) {
      Serial.println("Turning on Pump");
    }
    pin4PumpStatus = "on";
  }
  digitalWrite(relay[trafficCop], relayOn);
  if (isDebugEnabled) {
    Serial.print("Turning on Relay ");
    Serial.println(relay[trafficCop]);
  }

  t.stop(stationTimer[trafficCop]); // Kill any previously started timers.
  stationTimer[trafficCop] = t.after (stationTime[trafficCop],toggleOff);
  scheduleUpdate();
  schedulePumpUpdate();

  if (isDebugEnabled) {
    Serial.println("Ending toggleOn");
  }
}
void toggleOff() {
  if (isDebugEnabled) {
    Serial.println("Starting toggleOff");
  }
  digitalWrite(relay[trafficCop], relayOff);
  if (isDebugEnabled) {
    Serial.print("Turning off Relay ");
    Serial.println(relay[trafficCop]);
  }

  
  queue[trafficCop]=0; //remove relay from from queue
  if (maxvalue() ==0) {
    if (isConfigPump || isPin4Pump) {
      int8_t pumpOffEvent = t.after(pumpOffDelay*60L*1000L, pumpOff);  //turns off pump after delay
    }  
  }
  trafficCop=0; //ready to check queue or watch for new commmonds
  scheduleUpdate();
  schedulePumpUpdate();
  if (isDebugEnabled) {
    Serial.println("Ending toggleOff");
  }
}

//added allOff as requirement of SmartThings switch capability.  
void allOff() {
  if (isDebugEnabled) {
    Serial.println("Starting allOff");
  }
  int i=1;
  while(i<stations+1) {
    queue[i]=0;
    digitalWrite(relay[i], relayOff);
    if (isDebugEnabled) {
      Serial.print("Turning off Relay ");
      Serial.println(relay[i]);
    }

    i++;
  }
  if (isConfigPump || isPin4Pump) {
    int8_t pumpOffEvent = t.after(pumpOffDelay*60L*1000L, pumpOff);  //turns off pump after delay
  } 
  
  trafficCop=0;
  scheduleUpdate();
  schedulePumpUpdate();
  if (isDebugEnabled) {
    Serial.println("Ending allOff");
  }
}

void pumpOff() {
  if (isDebugEnabled) {
    Serial.println("Starting pumpOff");
  }
  if (isConfigPump) { 
      digitalWrite(relay[8], relayOff); 
      if (isDebugEnabled) {
        Serial.println("Turning off Pump");
      }

      configPumpStatus = "off";
  }
  if (isPin4Pump) {
      digitalWrite(pin4Relay, relayOff);
      if (isDebugEnabled) {
        Serial.println("Turning off Pump");
      }

      pin4PumpStatus="off";
  }
  schedulePumpUpdate();
  if (isDebugEnabled) {
    Serial.println("Ending pumpOff");
  }
}

void pumpOn() {
  if (isDebugEnabled) {
    Serial.println("Starting pumpOn");
  }
  if (isConfigPump) { 
      digitalWrite(relay[8], relayOn); 
      if (isDebugEnabled) {
        Serial.println("Turn on Pump");
      }

      configPumpStatus = "on";
  }
  if (isPin4Pump) {
      digitalWrite(pin4Relay, relayOn);
      if (isDebugEnabled) {
        Serial.println("Turn on Pump");
      }

      pin4PumpStatus = "on";
  }
  if (isDebugEnabled) {
    Serial.println("Ending pumpOn");
  }
}

void timeToUpdate() {
  if (isDebugEnabled) {
    Serial.println("Starting timeToUpdate");
  }
  scheduleUpdate();
  schedulePumpUpdate();
  if (isDebugEnabled) {
    Serial.println("Ending timeToUpdate");
  }
}

int maxvalue () {
  if (isDebugEnabled) {
    Serial.println("Starting maxvalue");
  }
  int mxm = 0;
  int i=1;
  for (i=1; i<stations+1; i++) {
    if (queue[i]>mxm) {
      mxm = queue[i];
    }
  }
  return mxm;
  if (isDebugEnabled) {
    Serial.println("Ending maxvalue");
  }
};

// send json data to client connection
void sendJSONData(WiFiClient client) {
  String updateStatus = makeUpdate();
  client.println(F("CONTENT-TYPE: application/json"));
  //client.println(F("CONTENT-LENGTH: 29"));
  client.println();
  client.println(updateStatus);
}

// send data
int sendNotify() //client function to send/receieve POST data.
{
  int returnStatus = 1;
  if (client.connect(hubIp, hubPort)) {
    client.println(F("POST / HTTP/1.1"));
    client.print(F("HOST: "));
    client.print(hubIp);
    client.print(F(":"));
    client.println(hubPort);
    sendJSONData(client);
    if (isDebugEnabled) {
      Serial.println(F("Pushing new values to hub..."));
    }
  }
  else {
    //connection failed
    returnStatus = 0;
    if (isDebugEnabled) {
      Serial.println(F("Connection to hub failed."));
    }
  }

  // read any data returned from the POST
  while(client.connected() && !client.available()) delay(1); //waits for data
  while (client.connected() || client.available()) { //connected or data available
    char c = client.read();
  }

  delay(1);
  client.stop();
  return returnStatus;
}

void sendUpdate(String statusUpdate) {
  if (isDebugEnabled) {
    Serial.println("Starting sendUpdate");
  }

  sendNotify();
  
  if (isDebugEnabled) {
    Serial.println("Ending sendUpdate ");
  }
}

String makeUpdate() {
  if (isDebugEnabled) {
    Serial.println("Starting makeUpdate");
  }

  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  JsonObject& rel = json.createNestedObject("relay");
    
  // builds a status update to send to SmartThings hub
  String action="";
  String key="";
  String number="";
  String statusUpdate="";
  for (int i=1; i<stations+1; i++) {
    if (i==1){
      key="zoneOne";
    }
    if (i==2){
      key="zoneTwo";
    }
    if (i==3){
      key="zoneThree";
    }
    if (i==4){
      key="zoneFour";
    }
    if (i==5){
      key="zoneFive";
    }
    if (i==6){
      key="zoneSix";
    }
    if (i==7){
      key="zoneSeven";
    }
    if (i==8){
      key="zoneEight";
    }
    
    if (queue[i]==0) {
      action="off";
    }
    if (queue[i]==1) {
      action="q";
    }
    if (queue[i]==2) {
      action="on";
    }

    //rel["name"] = key;
    //rel["action"] = action;
    rel[key] = action;
    key="";
  }

  if (isConfigPump && configPumpStatus == "enabled") {
    json["pump"] = "pumpAdded";
  }
  if (!isConfigPump && !isPin4Pump) {
    json["pump"] = "pumpRemoved";
  }
  if (configPumpStatus == "on" || pin4PumpStatus == "on") {
    json["pump"] = "onPump";
  }
  if (configPumpStatus =="off" && isConfigPump) {
    json["pump"] = "offPump";
  }
  if (pin4PumpStatus =="off" && isPin4Pump) {
    json["pump"] = "offPump";
  }
  
  json["version"] = version;

  if (hubPort != 0) {
    json["configuration"] = "true";
  }else {
    json["configuration"] = "false";
  }
  
  if (isDebugEnabled) {
    Serial.print("JSON ");
    json.printTo(Serial);
  }
  json.printTo(statusUpdate);
  
  if (isDebugEnabled) {
    Serial.print("Created Update Message ");
    Serial.println(statusUpdate);
  }
    if (isDebugEnabled) {
    Serial.println("Ending makeUpdate ");
  }
  return statusUpdate;
}

void sendPumpUpdate() {
  if (isDebugEnabled) {
    Serial.println("Starting sendPumpUpdate");
  }
  if (isConfigPump && configPumpStatus == "enabled") {
    //smartthing.send("pumpAdded");
  }
  if (!isConfigPump && !isPin4Pump) {
    //smartthing.send("pumpRemoved");
  }
  if (configPumpStatus == "on" || pin4PumpStatus == "on") {
    //smartthing.send("onPump");
  }
  if (configPumpStatus =="off" && isConfigPump) {
    //smartthing.send("offPump");
  }
  if (pin4PumpStatus =="off" && isPin4Pump) {
    //smartthing.send("offPump");
  }
  if (isDebugEnabled) {
    Serial.println("Ending sendPumpUpdate");
  }
}


bool loadAppConfig() {
  File configFile = SPIFFS.open(FPSTR(APPSETTINGS), "r");
  if (!configFile) {
    if (isDebugEnabled) {
      Serial.println(F("Failed to open config file"));
    }
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    if (isDebugEnabled) {
      Serial.println(F("Config file size is too large"));
    }
    return false;
  }

  std::unique_ptr<char[]> buf(new char[size]);
  configFile.readBytes(buf.get(), size);
  configFile.close();

  const int BUFFER_SIZE = JSON_OBJECT_SIZE(3);
  StaticJsonBuffer<BUFFER_SIZE> jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    if (isDebugEnabled) {
      Serial.println(F("Failed to parse application config file"));
    }
    return false;
  }

  hubPort = json[FPSTR(HUBPORT)];
  
  String hubAddress = json[FPSTR(HUPIP)];
  
  hubIp = IPfromString(hubAddress);
  String savedDeviceName = json[FPSTR(DEVICENAME)];
  deviceName = savedDeviceName;
  if (isDebugEnabled) {
    Serial.print(FPSTR(HUBPORT));
    Serial.print(FPSTR(LOADED));
    Serial.println(hubPort);
    Serial.print(FPSTR(HUPIP));
    Serial.print(FPSTR(LOADED));
    Serial.println(hubAddress);
    Serial.print(FPSTR(DEVICENAME));
    Serial.print(FPSTR(LOADED));
    Serial.println(deviceName);
  }
  return true;
}

bool saveAppConfig(String jsonString) {
  if (isDebugEnabled) {
    Serial.print(F("Saving new settings: "));
    Serial.println(jsonString);
  }
  File configFile = SPIFFS.open(FPSTR(APPSETTINGS), "w");
  if (!configFile) {
    if (isDebugEnabled) {
      Serial.println(F("Failed to open application config file for writing"));
    }
    return false;
  }
  configFile.print(jsonString);
  configFile.close();
  return true;
}

