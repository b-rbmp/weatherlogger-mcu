
#include <ArduinoJson.h>

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <TZ.h>
#include <FS.h>
#include <LittleFS.h>
#include <CertStoreBearSSL.h>

#include "Arduino.h"
#include <DHT12.h>
#include <Adafruit_BMP280.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// Configurações da Estação
const char* api_key = "teste";

//WiFi information
const char* ssid = "WIFI_SSID";
const char* password = "WIFI_PASS";
 
// Servidor MQTT
const char* mqtt_server = "MQTT_SERVER";
const int mqttPort = 8883;
const char* topico = "weatherlogger/medidas";
const char* mqttUser = "MQTT_USER";
const char* mqttPass = "MQTT_PASS";

// A single, global CertStore which can be used by all connections.
// Needs to stay live the entire time any of the WiFiClientBearSSLs
// are present.
BearSSL::CertStore certStore;

WiFiClientSecure espClient;
PubSubClient * client;

// Datas
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0);
String formattedDate;

unsigned long lastMsg = 0;
unsigned long lastReading = 0;
int nMedicoesEntreComm = 0;

// Intervalos
const int intervaloLeitura = 5000; // 5s (Padrão INMET) - Estação Meteorológica Automática (EMA)
const int intervaloEnvio = 60000*15; // 15 minutos - Padrão Weather.GOV estações extraoficiais (INMET é 1 hora)

// Medicoes Instantaneas
float t12 = 0;
float h12 = 0;
int co2ppm = 0;
int val_d = 0;
char *rain = "nao";
float hic12 = 0; 
float dpc12 = 0;
float tbme280 = 0;
float pbme280 = 0;

// Medicoes Médias
float t12med = 0;
float h12med = 0;
int co2ppmmed = 0;
float tbme280med = 0;
float pbme280medhpa = 0;
float hic12med = 0; 
float dpc12med = 0;
  
#define MSG_BUFFER_SIZE (500)

//Constants
#define anInput A0 //analog feed from MQ135
#define co2Zero 55 //calibrated CO2 0 level
#define pin_d 0 //digital feed from FC37

//Set dht12 i2c comunication on default wire pin
DHT12 dht12;

Adafruit_BMP280 bme; // I2C

// Setup do Wifi
void setup_wifi() {
  delay(10);
  // We start by connecting to a WiFi network
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  randomSeed(micros());

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

}

void setDateTime() {
  configTime(TZ_America_Sao_Paulo, "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(100);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();

  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.printf("%s %s", tzname[0], asctime(&timeinfo));
}


void reconnect() {
  while (!client->connected()) {
    Serial.print("Estabelecendo conexão MQTT");
    String clientId = "ESP8266Client - WeatherLogger";
    if (client->connect(clientId.c_str(), mqttUser, mqttPass)) {
    } else {
      Serial.print("Erro, rc = ");
      Serial.print(client->state());
      Serial.println(" Nova tentativa em 5 segundos");
      delay(5000);
    }
  }
}


 
void setup()
{
  delay(500);
  // Inicializa a comunicação Serial
  Serial.begin(9600);
  delay(500);

  LittleFS.begin();
  setup_wifi();
  setDateTime();
  // Inicializa o cliente de tempo em UTC-0
  timeClient.begin(); 
  while(!timeClient.update()) {
    timeClient.forceUpdate();
  }
  // Setup Pins
  pinMode(pin_d, INPUT);
  pinMode(anInput,INPUT);


  // Começa o sensor DHT12
  dht12.begin();

  // Começa o sensor BME280
  bme.begin(0x76);


  // you can use the insecure mode, when you want to avoid the certificates
  //espclient->setInsecure();

  int numCerts = certStore.initCertStore(LittleFS, PSTR("/certs.idx"), PSTR("/certs.ar"));
  Serial.printf("Number of CA certs read: %d\n", numCerts);
  if (numCerts == 0) {
    Serial.printf("No certs found. Did you run certs-from-mozilla.py and upload the LittleFS directory before running?\n");
    return; // Can't connect to anything w/o certs!
  }

  BearSSL::WiFiClientSecure *bear = new BearSSL::WiFiClientSecure();
  // Integrate the cert store with this connection
  bear->setCertStore(&certStore);

  client = new PubSubClient(*bear);

  client->setServer(mqtt_server, mqttPort);
  //client->setCallback(callback);

}


void loop()
{

  timeClient.update();
  
  if (!client->connected()) {
    reconnect();
  }
  client->loop();
  
  unsigned long now = millis();

  // Leitura não-bloquante a cada 5s
  if (now - lastReading > intervaloLeitura) {
    nMedicoesEntreComm++;
    lastReading = now;
    t12 += dht12.readTemperature();
    h12 += dht12.readHumidity();
    co2ppm += analogRead(A0) - co2Zero;
    tbme280 += bme.readTemperature();
    pbme280 += bme.readPressure();
    val_d = digitalRead(pin_d);
    
    if(val_d)
    {
      rain = "nao";
    }
    else
    {
      rain = "sim";
    }
  }

  // Envio de dados de 15 em 15 minutos
  if (now - lastMsg > intervaloEnvio) {
    lastMsg = now;

    
    StaticJsonBuffer<MSG_BUFFER_SIZE> JSONbuffer;
    JsonObject& JSONencoder = JSONbuffer.createObject();

    t12med = t12/nMedicoesEntreComm;
    h12med = h12/nMedicoesEntreComm;
    co2ppmmed = co2ppm/nMedicoesEntreComm;
  
    //tbme280med = tbme280/nMedicoesEntreComm;
    pbme280medhpa = (pbme280/nMedicoesEntreComm)/100;
  
    // Compute heat index in celsius
    hic12 = dht12.computeHeatIndex(t12med, h12med, false);
    // Compute dew point in celsius
    dpc12 = dht12.dewPoint(t12med, h12med, false);

    // Gera a Mensagem
    JSONencoder["api_key"] = api_key;


    formattedDate = timeClient.getFormattedDate();
    int splitT = formattedDate.indexOf("T");
    int splitZ = formattedDate.indexOf("Z");
    formattedDate[splitT] = ' ';
    formattedDate = formattedDate.substring(0, splitZ);
    JSONencoder["date"] = formattedDate;
    
    JSONencoder["temperature"] = t12med;
    JSONencoder["heat_index"] = hic12;
    JSONencoder["dewpoint"] = dpc12;
    JSONencoder["humidity"] = h12med;
    JSONencoder["pressure"] = pbme280medhpa;
    JSONencoder["dioxide_carbon_ppm"] = co2ppmmed;
    JSONencoder["rain_presence"] = rain;

    char JSONmessageBuffer[MSG_BUFFER_SIZE];
    JSONencoder.printTo(JSONmessageBuffer, sizeof(JSONmessageBuffer));
    
    Serial.print("Publish message: ");
    Serial.println(JSONmessageBuffer);
    client->publish(topico, JSONmessageBuffer);

    t12 = 0;
    h12 = 0;
    co2ppm = 0;
    tbme280 = 0;
    pbme280 = 0;

    nMedicoesEntreComm = 0;
  }
  
}
