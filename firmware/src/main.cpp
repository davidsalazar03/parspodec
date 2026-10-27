#include <Arduino.h>
#include "secrets.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// FSM LOGIC -----------------------

enum class FSMState {INIT, IDLE, PROBING, SWITCH};

typedef struct {
  FSMState state{FSMState::INIT};
  FSMState new_state{FSMState::INIT};
  unsigned long tes, tis;
} fsm_t;

void set_state(fsm_t& fsm, FSMState new_state)
{
  if (fsm.state != new_state) {  // if the state changed tis is reset
    fsm.state = new_state;
    fsm.tes = millis();
    fsm.tis = 0;
  }
}

fsm_t spots_fsm;

// PINS --------------------------
int trigPin = 5;       //TRIG pin do HC-SR04 - jumpers amarelo + verde
int echoPin = 18;      //ECHO pin do HC-SR04 - jumpers laranja + azul

// CONSTANTS
const int N_SAMPLES = 4;
const int BAUDRATE = 115200;
const unsigned long ECHO_TIMEOUT_US = 30000UL;

// GLOBALS -----------------------
float currDistance = 0.0;
float distances[N_SAMPLES] = {0};
bool SEND_NEW_STATE = false;
unsigned long READ_INTERVAL_MS = 1000UL;   // 1 s
unsigned long lastRead = 0;

// Spots codes: -1 -> unkown; 0 -> empty; 1 -> taken;
int L1 = -1;
int L2 = -1;
float spot_limits[] = {120.0, 180.0, 350.0};    // in cm
int current_d_code = -1;
int last_d_code = -1;
int candidate_d_code = -1;
unsigned long time_threshold_MS = 30000UL;     // 30s

// In the INIT State, the system computes the average distance for 5s before deciding what the initial spot's state should be
float INIT_avg_dist = 0.0;  // cm
int INIT_count_dist = 0;

// PROTOTYPES --------------------
float sampleDistance();
int evalute_sensor_reading(float d);
void update_spots_state(int code);

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

// Envia o estado atual de L1 e L2 para o Supabase (tabela spot_states).
// Devolve true se o servidor confirmou a gravação (HTTP 201).
bool sendReading(){
  connect_wifi();
  if(WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, SUPABASE_URL)){
    Serial.println("[HTTP] http.begin falhou");
    return false;
  }

  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  // Um único objeto JSON, com os nomes das colunas em minúsculas: {"l1":1,"l2":-1}
  String body = "{\"l1\":" + String(L1) + ",\"l2\":" + String(L2) + "}";

  int code = http.POST(body);
  Serial.printf("POST %s -> %d\n", body.c_str(), code);

  if (code != 201) Serial.println(http.getString());
  http.end();

  return code == 201;
}

// DISTANCE ------------------------------------------------------------

//Returns the average of the last 5 distance readings from the sensor 
float compute_latest_distance(){
  float dist_sum;
  int valid = 0;
  
  for (int i = 0; i < N_SAMPLES; i++){
    float latest_dist = sampleDistance();
    if (latest_dist>= 20.0){
      dist_sum += latest_dist;
      valid++;
    } 
    delay(50);
  }

  if (valid == 0) return 999.0;
  return dist_sum / valid;

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


// SPOTS LOGIC -----------------

void run_fsm_logic(float currDist){

  spots_fsm.tis = millis() - spots_fsm.tes;

  int last_d_code = evalute_sensor_reading(currDist); 

  // 1. Update Transitions
  switch (spots_fsm.state)
  {
  case FSMState::INIT:
    if (spots_fsm.tis >= 5000 && INIT_count_dist >= 10) {
      spots_fsm.new_state = FSMState::SWITCH;
      candidate_d_code = evalute_sensor_reading(INIT_avg_dist);
    }
    break;
  
  case FSMState::IDLE:
    if (last_d_code != current_d_code) {
      spots_fsm.new_state = FSMState::PROBING; 
      candidate_d_code = last_d_code;
    }
    break;

  case FSMState::PROBING:
    if (last_d_code != candidate_d_code) {spots_fsm.new_state = FSMState::IDLE;}
    else if (spots_fsm.tis >= time_threshold_MS) {spots_fsm.new_state = FSMState::SWITCH;}
    break;

  case FSMState::SWITCH:
    spots_fsm.new_state = FSMState::IDLE;
    break;
  }

  // 2. State Transition
  set_state(spots_fsm, spots_fsm.new_state);

  // 3. Perform Actions
  switch (spots_fsm.state)
  {
  case FSMState::INIT:
    current_d_code = -1;
    if (last_d_code >= 0) {
      INIT_count_dist ++;
      INIT_avg_dist = (INIT_avg_dist * (INIT_count_dist - 1) + currDist) /  INIT_count_dist;  
    }  
    break;
  
  case FSMState::IDLE:
    SEND_NEW_STATE = false;
    break;

  case FSMState::PROBING:
    break;

  case FSMState::SWITCH:
    current_d_code = candidate_d_code; 
    update_spots_state(current_d_code);
    SEND_NEW_STATE = true;
    break;

  }

}

// Return dictionary: -1 = under min limit; 0 = L1 taken, L2 unkown; 1 = L1 empty, L2 taken; 2 = L1 and L2 empty;
int evalute_sensor_reading(float d){

  if (d <= spot_limits[0]) {return -1;}
  else if (d > spot_limits[0] && d <= spot_limits[1]) {return 0;}
  else if (d > spot_limits[1] && d <= spot_limits[2]) {return 1;}
  else {return 2;}

}

// Actually change the spot's state variables
void update_spots_state(int code){   

  switch (code)
  {
    case -1:
      L1 = -1; L2 = -1;
      break;
    
    case 0:
      L1 = 1; L2 = -1;
      break;
    
    case 1:
      L1 = 0; L2 = 1;
      break;

    case 2:
      L1 = 0; L2 = 0;
      break;

  }

}


//--------------------------------
void setup () {

  Serial.begin(BAUDRATE);
  
  //pinMode(ledPin, OUTPUT);
  pinMode(trigPin, OUTPUT);           //Envia um impulso
  pinMode(echoPin, INPUT);            //Recebe o impulso refletido
  digitalWrite(trigPin, LOW);

  connect_wifi();

}

void loop () {

  if (millis() - lastRead >= READ_INTERVAL_MS){
    currDistance = compute_latest_distance();

    Serial.print("Current distance (cm): ");
    Serial.println(currDistance);
    
    run_fsm_logic(currDistance);

    lastRead = millis();
  }

  if (SEND_NEW_STATE && sendReading()){
    SEND_NEW_STATE == false;
  }

}
