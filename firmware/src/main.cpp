#include <Arduino.h>
#include "secrets.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>


// PINS --------------------------
int trigPin = 5;       //TRIG pin do HC-SR04 - jumpers amarelo + verde
int echoPin = 18;      //ECHO pin do HC-SR04 - jumpers laranja + azul

// CONSTANTS
const int N_SAMPLES = 5;
const int BAUDRATE = 115200;
const unsigned long ECHO_TIMEOUT_US = 30000UL;

// GLOBALS -----------------------
float distances[N_SAMPLES] = {0};
float speeds[N_SAMPLES] = {0};
const unsigned long SEND_INTERVAL_MS = 10000UL;   // 10 s
unsigned long lastSend = 0;

// Spots codes: -1 -> unkown; 0 -> empty; 1 -> taken;
int L1 = -1;
int L2 = -1;
float spot_limits[] = {120.0, 170.0, 250.0, 350.0, 400.0};    // in cm

// PROTOTYPES --------------------
float sampleDistance();
float sampleSpeed();

// WiFi --------------------------
void connect_wifi(){
  if(WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  Serial.print("A conectar ao Wi-Fi...");

  unsigned long t0 = millis();
  
  while(WiFi.status() != WL_CONNECTED && (millis() - t0 < 15000)){
    delay(500);
    Serial.print(".");
  }

  Serial.println(WiFi.status() == WL_CONNECTED ? "[WIFI RESULT] CONEXÃO COMPLETA " : "[WIFI RESULT] CONEXÃO FALHOU");

}

void sendReading(float d){
  connect_wifi();
  if(WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, SUPABASE_URL)){
    Serial.println("Conexão ao HTTTP falhou");
    http.end();
    return;
  }

  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  String body = "{\"distance_cm\":" + String(d, 1) + "}";

  int code = http.POST(body);
  Serial.printf("POST %s -> %d\n", body.c_str(), code);

  if (code != 201) Serial.println(http.getString());
  http.end();

}

// DISTANCE ------------------------------------------------------------

//Updates the last 5 distance readings from the sensor with the newest reading from SAMPLE_DISTANCE(and removes the oldest one)
void update_distance_FIR(){
  for (int i = N_SAMPLES-1; i > 0 ; i--){
    distances[i] = distances[i-1];
  }

  delay(40);        // delay necessário para o sensor AJ-SR04 (não necessário para o HC-SR04)
  float new_distance = sampleDistance();

  if (new_distance > 0.0){
    distances[0] = new_distance;
  } else {
    distances[0] = distances[1];
  }

}

//Returns the average of the last 5 distance readings from the sensor 
float output_distance_FIR(){
  float cur_distance = 0;
  
  for (int i = 0; i < N_SAMPLES; i++){
    cur_distance += distances[i];
  }

  return cur_distance / N_SAMPLES;

}

//Calculates the current distance by triggering the sensor and reading the echo of the reflected sound waves
float sampleDistance(){
  
  digitalWrite(trigPin,LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin,HIGH);
  delayMicroseconds(15);
  digitalWrite(trigPin,LOW);    

  unsigned long elapsed_t = pulseIn(echoPin, HIGH, ECHO_TIMEOUT_US);

  if (elapsed_t == 0){
    return -1.0;
  }
  return (elapsed_t / 29.15) / 2;    // distance in cm

}

// SPEED ---------------------------------------------------------------

//Updates the last 5 speed readings with the newest value from SAMPLE_SPEED (and removes the oldest one)
void run_speed_FIR(){
  for (int i = N_SAMPLES -1; i > 0;i--){
    speeds[i] = speeds[i-1];
  }

  speeds[0] = sampleSpeed();

}

//Returns the average of the last 5 speed readings
float output_speed_FIR(){
  float cur_speed = 0;
  
  for (int i = 0; i < N_SAMPLES; i++){
    cur_speed += speeds[i];
  }

  return cur_speed / N_SAMPLES;

}

//Calculates the current speed by measuring the difference in distance over a certain time period using GET_DISTANCE
float sampleSpeed(){
  
  float f_distance = output_distance_FIR();
  unsigned long f_time = millis();
  
  update_distance_FIR(); 
  delay(50);

  float s_distance = output_distance_FIR();
  unsigned long s_time = millis();
  
  float dist_diff = fabs(s_distance - f_distance);
  float elapsed_time = s_time - f_time;

  if (elapsed_time == 0){
    return 0.0;
  }

  return (dist_diff / elapsed_time) * 1000;     // speed in cm/s
  
}

// SPOTS LOGIC -----------------

void update_spots_state(float d){

  if (d < spot_limits[0]) {L1 = -1; L2 = -1; }
  else if (d > spot_limits[0] && d < spot_limits[1]) {L1 = 1; L2 = -1;}
  else if (d > spot_limits[1] && d < spot_limits[2]) {L1 = 0; L2 = 1; }
  else if (d > spot_limits[2] && d < spot_limits[3]) {L1 = 0; L2 = 0; }
  else if (d > spot_limits[3]) {L1 = -1; L2 = -1;}

}

//--------------------------------
void setup () {

  Serial.begin(BAUDRATE);
  
  //pinMode(ledPin, OUTPUT);
  pinMode(trigPin, OUTPUT);           //Envia um impulso
  pinMode(echoPin, INPUT);            //Recebe o impulso refletido
  digitalWrite(trigPin, LOW);

  // Enche o buffer com leituras reais, para a média não começar "puxada" para 0
  for (int i = 0; i < N_SAMPLES; i++) {
    update_distance_FIR();
  }

  connect_wifi();

}

void loop () {

  update_distance_FIR();
  //run_speed_FIR();

  //float currSpeed = output_speed_FIR();
  float currDistance = output_distance_FIR();

  //Serial.print(currSpeed);
  //Serial.print(",");
  Serial.print("Current distance (cm): ");
  Serial.println(currDistance);
  
  update_spots_state(currDistance);

  if (millis() - lastSend >= SEND_INTERVAL_MS){
    sendReading(currDistance);
    lastSend = millis();
  }

  delay(50);
}
