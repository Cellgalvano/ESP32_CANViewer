/* 
 *  ESP32 CANViewer 
 *  V0.1
 *  Connect CRX to Pin 4, CTX to Pin 5
*/

#include <SPIFFS.h>
#include <CAN.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

const char* ssid = "YOURNETWORK";
const char* password = "YOURPASSWORD";

AsyncWebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(1337);

long CAN_baudRate = 500E3;
long CAN_filterId = -1;
long CAN_filterMask = -1;

void onWebSocketEvent(uint8_t client_num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", client_num);
      break;
      
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(client_num);
        Serial.printf("[%u] Connection from ", client_num);
        Serial.println(ip.toString());
      }
      break;

    case WStype_TEXT:
      {
        Serial.printf("[%u] Received text: %s\n", client_num, payload);
  
        StaticJsonDocument<250> jsonobj;
        DeserializationError err = deserializeJson(jsonobj, payload);
        if(err){
          Serial.println("Invalid JSON!");
        }else{
          const char* command = jsonobj["cmd"];
          
          Serial.print("CMD: ");
          Serial.println(command);

          // CMD: SETFILTER
          if(String(command).equals("SETFILTER")){
            const unsigned int filter_id = jsonobj["id"];
            const unsigned int filter_mask = jsonobj["mask"];
            CAN.filter(filter_id, filter_mask);
            CAN_filterId = filter_id;
            CAN_filterMask = filter_mask;
            Serial.printf("Setting Filter ID: %x ,MASK: %x\n", filter_id, filter_mask);
          }

          // CMD: SETSPEED
          if(String(command).equals("SETSPEED")){
            const unsigned int targetspeed = jsonobj["speed"];
            CAN_baudRate = targetspeed;
            CAN.end();
            CAN.begin(CAN_baudRate);
            Serial.printf("Setting Speed/Baudrate to: %u\n", targetspeed);
          } 

          // CMD: SEND
          if(String(command).equals("SEND")){
            const unsigned int send_id = jsonobj["id"];
            const unsigned int send_dlc = jsonobj["dlc"];
            JsonArray send_arr = jsonobj["data"].as<JsonArray>();
            if(send_dlc == send_arr.size()){
              if(send_id <= 0x7FF){
                CAN.beginPacket(send_id, send_dlc);
              }else{
                CAN.beginExtendedPacket(send_id, send_dlc);  
              }
              for(byte i=0; i<send_dlc; i++){
                CAN.write((byte)send_arr[i]);  
              }
              char can_err = CAN.endPacket();
              if(!can_err){
                Serial.println("ERR: Sending Frame!");  
              }
            }else{
              Serial.println("ERR: Size does not match!");  
            }
            Serial.printf("Sending Frame with ID: %x\n", send_id);
          }  
        } 
      }
      break;

    // Ignore all other types
    case WStype_BIN:
    case WStype_ERROR:
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
    default:
      break;
  }
}

void onIndexRequest(AsyncWebServerRequest *request) {
  IPAddress remote_ip = request->client()->remoteIP();
  Serial.println("["+remote_ip.toString()+"] HTTP GET request of " + request->url());
  request->send(SPIFFS, "/index.html", "text/html");
}

void onPageNotFound(AsyncWebServerRequest *request) {
  IPAddress remote_ip = request->client()->remoteIP();
  Serial.println("["+remote_ip.toString()+"] HTTP GET request of " + request->url());
  request->send(404, "text/plain", "Not found");
}

void setup() {
  Serial.begin(115200);
  while (!Serial);
  Serial.println("");
  Serial.println("ESP32 CANViewer");

  // Test SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // Connect to WIFI
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  WiFi.setHostname("ESP32CANViewer");
  Serial.print("Connecting to WIFI ...");
  byte wifi_counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    wifi_counter++;
    if(wifi_counter > 20){
      WiFi.disconnect();
      WiFi.mode(WIFI_AP);
      WiFi.softAP("ESP32CANViewer", NULL);  
      Serial.print("[Switching to AP Mode]");
      break;
    }
  }
  Serial.println(" connected!");
  
  // Show IP Address
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // Setup Webserver
  server.on("/", HTTP_GET, onIndexRequest);
  server.onNotFound(onPageNotFound);
  server.begin();

  // Setup Websocket Server
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  // Start CAN with default Baudrate
  if (!CAN.begin(CAN_baudRate)) {
    Serial.println("Starting CAN failed!");
    while (1);
  }else{
    Serial.println("CAN started!");  
  }
  
}

long prevMillis = 0;
void loop() {
  webSocket.loop();
  handleCAN_RX();

  // Broadcast STATUS message every second
  if((millis() - prevMillis) > 1000){
    prevMillis = millis();
    //Serial.println("Sending status!"); 
    StaticJsonDocument<250> jsonobj;
    String output = "";
    jsonobj["ts"] = millis();
    jsonobj["type"] = "STATUS";
    jsonobj["baudrate"] = CAN_baudRate;
    jsonobj["filterId"] = CAN_filterId;
    jsonobj["filterMask"] = CAN_filterMask;
    serializeJson(jsonobj, output);
    webSocket.broadcastTXT(output); 
  }
}

// Handle CAN Input
void handleCAN_RX() {
  
  int packetSize = CAN.parsePacket();
  if (packetSize) {

    StaticJsonDocument<250> jsonobj;
    String output = "";
    jsonobj["ts"] = millis();
    
    jsonobj["type"] = "STD";
  
    if (CAN.packetExtended()) {
      jsonobj["type"] = "EXT";
    }
  
    if (CAN.packetRtr()) {
      jsonobj["type"] = "RTR";
      if(CAN.packetExtended()) jsonobj["type"] = "ERTR";
    }

    jsonobj["id"] = CAN.packetId();
  
    if (CAN.packetRtr()) {
      // RTR FRAME
      jsonobj["dlc"] = CAN.packetDlc();
    } else {
      // NON RTR FRAME
      jsonobj["dlc"] = packetSize;
      JsonArray data = jsonobj.createNestedArray("data");
      while (CAN.available()) {
        char temp = (char)CAN.read();
        data.add(temp);
        //Serial.printf("%u\n", temp);
      }
      //Serial.printf("Size: %u\n", data.size());
    }

    serializeJson(jsonobj, output);
    webSocket.broadcastTXT(output); // Broadcast every received packet
  }
}
