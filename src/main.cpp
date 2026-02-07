#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

const char* WIFI_SSID     = "Paszymaja";
const char* WIFI_PASSWORD = "Pierogi123";
const char* VERSION       = "1.7.0-DEBUG-MODE";

WiFiUDP dnsUdp;
WiFiServer httpServer(80);
IPAddress myIP;
uint8_t dnsBuf[512]; 
String stationsXML;

void xmlEscape(String &s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("\"", "&quot;"); s.replace("'", "&apos;");
}

// ------------------------------------------------------------
// DIAGNOSTYKA POBIERANIA (Z WYPISANIEM TREŚCI BŁĘDU)
// ------------------------------------------------------------
void fetchStations() {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(10000);
  
  Serial.println("\n[DEBUG] Pobieranie stacji...");
  if (http.begin(client, "http://de1.api.radio-browser.info")) {
    http.setUserAgent("Mozilla/5.0");
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (!error) {
        String xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><ListOfItems>";
        int count = 0;
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject s : arr) {
          String name = s["name"] | "Radio";
          String url  = s["url_resolved"] | "";
          if (url.length() < 10) continue;
          xmlEscape(name); xmlEscape(url);
          xml += "<Item><ItemType>Station</ItemType><Title>" + name + "</Title><Link>" + url + "</Link></Item>";
          count++;
        }
        xml += "<ItemCount>" + String(count) + "</ItemCount></ListOfItems>";
        stationsXML = xml;
        Serial.printf("[DEBUG] Pobrano %d stacji.\n", count);
      } else {
        Serial.printf("[DEBUG] JSON Error: %s. Poczatek danych: %s\n", error.c_str(), payload.substring(0, 40).c_str());
      }
    } else {
      Serial.printf("[DEBUG] HTTP Error: %d\n", httpCode);
    }
    http.end();
  }
}

// ------------------------------------------------------------
// SZCZEGÓŁOWY MONITOR ZAPYTAŃ PIONIERA
// ------------------------------------------------------------
void handleHttp(WiFiClient &client) {
  unsigned long start = millis();
  while (client.available() == 0 && millis() - start < 2000);

  Serial.println("\n--- [ PIONEER INCOMING ] ---");
  String firstLine = "";
  bool firstLineRead = false;

  // Czytamy i wypisujemy KAŻDĄ linię nagłówka
  while (client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      Serial.println("H: " + line); // H jak Header
      if (!firstLineRead) { firstLine = line; firstLineRead = true; }
    }
  }
  Serial.println("--- [ END HEADERS ] ---");

  String body;
  String hostAddr = "http://" + myIP.toString();

  if (firstLine.indexOf("loginXML.asp") >= 0 || firstLine.indexOf("SessionXML.asp") >= 0) {
    body = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Login><Status>OK</Status><Token>0</Token><SessionID>88888888</SessionID><BrowseURL>" + hostAddr + "/setupapp/pioneer/asp/BrowseXML/browsexml.asp</BrowseURL></Login>";
    Serial.println("[ODP] Wyslano LoginXML");
  } 
  else if (firstLine.indexOf("browsexml.asp") >= 0) {
    body = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<ListOfItems><ItemCount>1</ItemCount><Item><ItemType>Dir</ItemType><Title>POLSKIE STACJE</Title><UrlDir>" + hostAddr + "/setupapp/pioneer/asp/BrowseXML/getstationxml.asp</UrlDir></Item></ListOfItems>";
    Serial.println("[ODP] Wyslano Menu");
  } 
  else if (firstLine.indexOf("getstationxml.asp") >= 0) {
    body = (stationsXML.length() > 0) ? stationsXML : "<?xml version=\"1.0\"?><ListOfItems><ItemCount>0</ItemCount></ListOfItems>";
    Serial.println("[ODP] Wyslano Liste Stacji");
  } else {
    Serial.println("[ODP] Nieznane - zamykam.");
    client.stop();
    return;
  }

  client.print("HTTP/1.0 200 OK\r\n");
  client.print("Content-Type: text/xml; charset=utf-8\r\n");
  client.print("Server: vtuner/1.0\r\n");
  client.printf("Content-Length: %d\r\n", body.length());
  client.print("Connection: close\r\n\r\n");
  client.print(body);
  client.stop();
}

void handleDNS() {
  int size = dnsUdp.parsePacket();
  if (size <= 0) return;
  dnsUdp.read(dnsBuf, 512);
  if (strstr((char*)dnsBuf, "vtuner") == NULL) return;

  dnsBuf[2] = 0x81; dnsBuf[3] = 0x80; dnsBuf[7] = 0x01;
  uint8_t ans[] = {0xC0,0x0C,0x00,0x01,0x00,0x01,0x00,0x00,0x00,0x3C,0x00,0x04,myIP[0],myIP[1],myIP[2],myIP[3]};
  dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort());
  dnsUdp.write(dnsBuf, size); dnsUdp.write(ans, 16); dnsUdp.endPacket();
  Serial.println("[DNS] Przekierowano zapytanie");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\n\nSTART yTuner %s\n", VERSION);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  myIP = WiFi.localIP();
  Serial.println("\nIP: " + myIP.toString());
  
  fetchStations();
  
  // Testowy XML jesli API zawiedzie
  if (stationsXML.length() < 50) {
    stationsXML = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><ListOfItems><ItemCount>1</ItemCount><Item><ItemType>Station</ItemType><Title>STACJA TESTOWA</Title><Link>http://stream.polskieradio.pl</Link></Item></ListOfItems>";
  }

  httpServer.begin();
  dnsUdp.begin(53);
}

void loop() {
  handleDNS();
  WiFiClient client = httpServer.available();
  if (client) handleHttp(client);
}
