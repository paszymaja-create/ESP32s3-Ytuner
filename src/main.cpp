#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ==================== KONFIGURACJA ====================
const char* WIFI_SSID     = "Paszymaja";
const char* WIFI_PASSWORD = "Pierogi123";

IPAddress currentIP;
WiFiUDP dnsUdp;
WiFiServer httpServer(80);
uint8_t dnsBuffer[512];
String stationsXML = "";

void escapeXml(String &s) {
    s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
    s.replace("\"", "&quot;"); s.replace("'", "&apos;");
}

// ==================== LOGOWANIE DO SERIALLY ====================
void logSection(String title) {
    Serial.println("\n-------------------------------------------");
    Serial.println("| " + title);
    Serial.println("-------------------------------------------");
}

// ==================== POBIERANIE STACJI ====================
void fetchStations() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://de1.api.radio-browser.info/json/stations/bycountrycodeexact/pl?limit=50&order=clickcount&reverse=true";
    
    Serial.println("[API] Start pobierania stacji...");
    if (http.begin(client, url)) {
        int httpCode = http.GET();
        if (httpCode == HTTP_CODE_OK) {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, http.getString());
            if (!error) {
                String xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ListOfItems>\n";
                int count = 0;
                for (JsonObject s : doc.as<JsonArray>()) {
                    String name = s["name"] | "Radio";
                    String s_url = s["url_resolved"] | "";
                    if (s_url.startsWith("http")) {
                        escapeXml(name); escapeXml(s_url);
                        xml += "  <Item>\n    <ItemType>Station</ItemType>\n";
                        xml += "    <StationName>" + name + "</StationName>\n";
                        xml += "    <StationUrl>" + s_url + "</StationUrl>\n";
                        xml += "    <StationFormat>mp3</StationFormat>\n  </Item>\n";
                        count++;
                    }
                }
                xml += "  <ItemCount>" + String(count) + "</ItemCount>\n</ListOfItems>";
                stationsXML = xml;
                Serial.printf("[API] Sukces: %d stacji załadowanych.\n", count);
            }
        } else {
            Serial.printf("[API] Błąd HTTP: %d\n", httpCode);
        }
        http.end();
    }
}

// ==================== OBSŁUGA HTTP Z PEŁNYM LOGOWANIEM ====================
void handleHttpClient(WiFiClient client) {
    if (!client.connected()) return;

    logSection("POŁĄCZENIE PRZYCHODZĄCE");
    
    String requestLine = "";
    if (client.available()) {
        requestLine = client.readStringUntil('\r');
        Serial.println("[REQ] " + requestLine);
    }

    // LOGOWANIE WSZYSTKICH NAGŁÓWKÓW
    while (client.available()) {
        String header = client.readStringUntil('\r');
        if (header == "\n") break;
        Serial.print("[H] " + header);
    }

    String response = "";
    String logType = "";

    if (requestLine.indexOf("token=0") >= 0) {
        logType = "FAZA 1: EncryptedToken";
        response = "<EncryptedToken>0123456789ABCDEF</EncryptedToken>";
    } 
    else if (requestLine.indexOf("loginXML.asp") >= 0) {
        logType = "FAZA 2: Login/Menu";
        response = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ListOfItems>\n"
                   "  <ItemCount>1</ItemCount>\n"
                   "  <Item>\n"
                   "    <ItemType>Dir</ItemType>\n"
                   "    <Title>MOJE RADIO (ESP32)</Title>\n"
                   "    <UrlDir>http://" + currentIP.toString() + "/ytuner/mystations</UrlDir>\n"
                   "    <UrlDirBackUp>http://" + currentIP.toString() + "/ytuner/mystations</UrlDirBackUp>\n"
                   "    <DirCount>50</DirCount>\n"
                   "  </Item>\n"
                   "</ListOfItems>";
    }
    else if (requestLine.indexOf("/ytuner/mystations") >= 0) {
        logType = "FAZA 3: Lista stacji";
        response = (stationsXML.length() > 0) ? stationsXML : "<?xml version=\"1.0\"?><Error>No Stations</Error>";
    }
    else {
        logType = "NIEZNANA ŚCIEŻKA";
    }

    Serial.println("[DECYZJA] " + logType);

    if (response.length() > 0) {
        client.print("HTTP/1.1 200 OK\r\n");
        client.print("Status: 200 OK\r\n");
        client.print("Content-Type: application/xml\r\n");
        client.print("Server: YTuner/1.2.6\r\n");
        client.print("Content-Length: " + String(response.length()) + "\r\n");
        client.print("Connection: close\r\n\r\n");
        client.print(response);
        Serial.println("[RESP] Wysłano bajtów: " + String(response.length()));
    }

    client.stop();
    Serial.println("-------------------------------------------");
}

// ==================== DNS Z LOGOWANIEM ZAPYTAŃ ====================
void handleDNS() {
    int packetSize = dnsUdp.parsePacket();
    if (packetSize <= 0) return;
    
    dnsUdp.read(dnsBuffer, 512);
    
    // Wyciąganie nazwy domeny z zapytania DNS
    String queriedDomain = "";
    for (int i = 13; i < packetSize - 4; i++) {
        char c = dnsBuffer[i];
        if (c > 31 && c < 127) queriedDomain += c;
        else queriedDomain += ".";
    }

    Serial.print("[DNS QUERY] " + queriedDomain);

    if (queriedDomain.indexOf("vtuner") >= 0 || queriedDomain.indexOf("pioneer") >= 0) {
        dnsBuffer[2] = 0x81; dnsBuffer[3] = 0x80; dnsBuffer[6] = 0x00; dnsBuffer[7] = 0x01;
        uint8_t answer[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04, currentIP[0], currentIP[1], currentIP[2], currentIP[3] };
        dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort());
        dnsUdp.write(dnsBuffer, packetSize);
        dnsUdp.write(answer, sizeof(answer));
        dnsUdp.endPacket();
        Serial.println(" -> PRZECHWYCONO!");
    } else {
        Serial.println(" -> POMINIĘTO");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    Serial.print("\n[WIFI] Łączenie");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    
    currentIP = WiFi.localIP();
    Serial.println("\n[WIFI] Połączono! IP: " + currentIP.toString());
    
    fetchStations();
    httpServer.begin();
    dnsUdp.begin(53);
    
    Serial.println("\n=== LOGGER AKTYWNY ===");
    Serial.println("1. Upewnij się, że Pioneer ma DNS: " + currentIP.toString());
    Serial.println("2. Wyłącz Raspberry Pi (YTuner)");
    Serial.println("3. Wyłącz radio z prądu na 10 sekund i włącz ponownie.");
    Serial.println("=======================");
}

void loop() {
    handleDNS();
    WiFiClient client = httpServer.available();
    if (client) handleHttpClient(client);
    
    static uint32_t lastF = 0;
    if (millis() - lastF > 3600000UL) { fetchStations(); lastF = millis(); }
}