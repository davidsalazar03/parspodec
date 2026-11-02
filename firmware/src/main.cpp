#include <Arduino.h>
#include "secrets.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#define DEEP_SLEEP_TIME 20   // in seconds

// FSM LOGIC -----------------------

enum class FSMState {INIT, IDLE, PROBING, SWITCH};

typedef struct {
  FSMState state;
  FSMState new_state;
  unsigned long ces, cis;
} fsm_t;

// <!> ----------- PERSISTENT MEMMORY VARIABLES ----------- <!> 

RTC_DATA_ATTR fsm_t spots_fsm = {FSMState::INIT, FSMState::INIT, 0, 0};     // FSM

RTC_DATA_ATTR unsigned int current_cycle_count = 0;

//RTC_DATA_ATTR unsigned long lastRead = 0;       // Time of last reading
//RTC_DATA_ATTR unsigned long lastReqPoll = 0;    // Time of last request pooling

RTC_DATA_ATTR int current_d_code = -1;          // Current d-code (defines the spot's state)
RTC_DATA_ATTR int last_d_code = -1;             // Last d-code
RTC_DATA_ATTR int candidate_d_code = -1;        // D-code candidate to switch with the current one

RTC_DATA_ATTR unsigned int LAST_REQ_SENT_CYCLE = 0;

RTC_DATA_ATTR bool SEND_NEW_STATE = false;  // True when the state of the spots changes and that info must be sent via WiFi

// Spots codes: -1 -> unkown; 0 -> empty; 1 -> taken;
RTC_DATA_ATTR int L1 = -1;
RTC_DATA_ATTR int L2 = -1;

// WiFi Connection Data
RTC_DATA_ATTR bool wifi_cache_valid = false;
RTC_DATA_ATTR int32_t  wifi_channel =     0;
RTC_DATA_ATTR uint8_t wifi_bssid[6] =   {0};

RTC_DATA_ATTR bool     ip_cache_valid = false;
RTC_DATA_ATTR uint32_t ip_local, ip_gw, ip_mask, ip_dns;
RTC_DATA_ATTR unsigned int ip_cached_cycle = 0;

// <!> ----------------------------------------------------- <!>        

// FSM Function to change states
void set_state(fsm_t& fsm, FSMState new_state)
{
  if (fsm.state != new_state) {  // if the state changed tis is reset
    fsm.state = new_state;
    fsm.ces = current_cycle_count;
    fsm.cis = 0;
  }
}

// PINS --------------------------
int trigPin = 5;       //TRIG pin do HC-SR04 - jumpers amarelo + verde
int echoPin = 18;      //ECHO pin do HC-SR04 - jumpers laranja + azul

// CONSTANTS
const int BAUDRATE = 115200;
const unsigned long ECHO_TIMEOUT_US = 30000UL;
const unsigned int NO_SENT_CYCLE_THRESHOLD = 5;
const unsigned int cycles_threshold_4switch = 3;     // 60s
const unsigned int IP_REFRESH_CYCLES = 43200 / DEEP_SLEEP_TIME;   // ~12h ajustadas em #ciclos


// GLOBALS -----------------------
float currDistance = 0.0;

// Debug: pedidos de distância vindos do site
float spot_limits[] = {120.0, 180.0, 350.0};    // in cm

float WAKEUP_TIME = 0.0;

// PROTOTYPES --------------------
float sampleDistance();
void go_to_deep_sleep();
int evalute_sensor_reading(float d);
void update_spots_state(int code);

// DeepSleep ---------------------

void go_to_deep_sleep(){
  Serial.print("Remained active for "); Serial.print(millis() - WAKEUP_TIME); Serial.println(" ms.");
  Serial.println(">>> Entering DEEP SLEEP Mode. ");
  Serial.flush();
  esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIME * 1000000); 
  esp_deep_sleep_start();
}

// WiFi --------------------------

// --- IP fixo ---
IPAddress local_IP(192, 168, 1, 73);
IPAddress gateway (192, 168, 1, 1);
IPAddress subnet  (255, 255, 255, 0);
IPAddress dns     (192, 168, 1, 1); 

bool wait_for_wifi(unsigned long timeout_ms){
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) delay(20);
  return WiFi.status() == WL_CONNECTED;
}

void connect_wifi(){

  if(WiFi.status() == WL_CONNECTED) return;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  // Usa o IP guardado só se existir e ainda não estiver "velho"
  bool use_static = ip_cache_valid && (current_cycle_count - ip_cached_cycle < IP_REFRESH_CYCLES);

  if (use_static){
    WiFi.config(IPAddress(ip_local), IPAddress(ip_gw),
                IPAddress(ip_mask),  IPAddress(ip_dns));
  } else {
    Serial.println("[WIFI] Using DHCP to obtain a new IP Address...");
  }

  unsigned long t0 = millis();
  bool ok = false;

  // 1) QUICK Connection: wifi channel + BSSID already known (smaller waiting time)
  if (wifi_cache_valid){
    WiFi.begin(WIFI_SSID, WIFI_PASS, wifi_channel, wifi_bssid);
    ok = wait_for_wifi(3000);

    if (!ok){ 
      WiFi.disconnect(); 
      wifi_cache_valid = false; 
      ip_cache_valid = false;
      if(use_static){
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);   // Invalidate the IP Address as well so that the next WiFi connection doesnt use the old one
        use_static = false;
      }
    }  

  }

  // 2) NORMAL Connection (1st boot or failure on the QUICK connection) (wifi channel/bssid unkonwn, bigger waiting time)
  if (!ok){
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    ok = wait_for_wifi(15000);
  }

  // 3) Store the wifi channel + BSSID for the next connection
  if (ok){
    wifi_channel = WiFi.channel();
    memcpy(wifi_bssid, WiFi.BSSID(), 6);
    wifi_cache_valid = true;
    Serial.printf("[WIFI] RSSI = %d dBm\n", WiFi.RSSI());
  }

  // 4) Store the ip cache after a DHCP Call for the next connections
  if (ok && !use_static){          
    ip_local = WiFi.localIP();
    ip_gw    = WiFi.gatewayIP();
    ip_mask  = WiFi.subnetMask();
    ip_dns   = WiFi.dnsIP();
    ip_cached_cycle = current_cycle_count;
    ip_cache_valid  = true;
  }

  Serial.printf("[WIFI] %s em %lu ms\n", ok ? "OK" : "FALHOU", millis() - t0);


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

// DEBUG: PEDIDOS DE DISTÂNCIA (vindos do site) -----------------------
// O site não consegue falar diretamente com o ESP32 (está atrás do router),
// por isso escreve um "pedido" na tabela distance_requests e o ESP32 vai lá
// verificar de REQ_POLL_INTERVAL_MS em REQ_POLL_INTERVAL_MS.

// Pergunta ao Supabase se há algum pedido por responder. Devolve true se houver.
bool checkDistanceRequest(){

  connect_wifi();
  if (WiFi.status() != WL_CONNECTED) return false;   // não bloqueia à espera do Wi-Fi

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  // Basta saber se existe pelo menos 1 pedido com answered_at vazio
  String url = String(SUPABASE_REQ_URL) + "?answered_at=is.null&select=id&limit=1";
  if (!http.begin(client, url)){
    Serial.println("[REQ] http.begin falhou");
    return false;
  }
  http.addHeader("apikey", SUPABASE_KEY);

  unsigned long t = millis();
  int code = http.GET();
  Serial.printf("[REQ] GET -> %d em %lu ms\n", code, millis() - t);

  String payload = (code == 200) ? http.getString() : "";
  http.end();

  if (code != 200){
    Serial.printf("[REQ] GET -> %d\n", code);
    return false;
  }
  return payload.length() > 2;   // "[]" = sem pedidos; "[{\"id\":7}]" = há pedido
}

// Responde a todos os pedidos pendentes com a distância indicada (em cm).
// Devolve true se o servidor aceitou (HTTP 204).
bool sendDistance(float d){

  connect_wifi();

  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(SUPABASE_REQ_URL) + "?answered_at=is.null";
  if (!http.begin(client, url)){
    Serial.println("[REQ] http.begin falhou");
    return false;
  }
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  // A hora da resposta (answered_at) é preenchida pelo servidor
  String body = "{\"distance_cm\":" + String(d, 1) + "}";

  unsigned long t = millis();
  int code = http.PATCH(body);

  Serial.printf("[REQ] PATCH %s -> %d em %lu ms\n", body.c_str(), code, millis() - t);

  if (code != 204) Serial.println(http.getString());
  http.end();

  return code == 204;
}

// DISTANCE ------------------------------------------------------------

//Returns the average of the last 5 distance readings from the sensor 
float compute_latest_distance(int N_SAMPLES){
  float dist_sum = 0;
  int valid = 0;
  
  for (int i = 0; i < N_SAMPLES; i++){
    float latest_dist = sampleDistance();
    Serial.printf("raw: %.1f\n", latest_dist);
    if (latest_dist >= 10.0){
      dist_sum += latest_dist;
      valid++;
    } 
    delay(50);
  }

  if (valid < floor(N_SAMPLES/2)) return -1.0;
  return dist_sum / valid;

}

//Calculates the current distance by triggering the sensor and reading the echo of the reflected sound waves
float sampleDistance(){
  
  digitalWrite(trigPin,LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin,HIGH);
  delayMicroseconds(20);
  digitalWrite(trigPin,LOW);    

  unsigned long elapsed_t = pulseIn(echoPin, HIGH, ECHO_TIMEOUT_US);

  delayMicroseconds(20);

  if (elapsed_t == 0){
    return -1.0;
  }
  return (elapsed_t / 29.15) / 2;    // distance in cm

}

// SPOTS LOGIC -----------------

void run_fsm_logic(float currDist){

  current_cycle_count++;
  spots_fsm.cis = current_cycle_count - spots_fsm.ces;

  last_d_code = evalute_sensor_reading(currDist); 

  Serial.print("["); Serial.print(current_cycle_count); Serial.print("] ");
  Serial.print("LAST state = ");
  

  // 1. Update Transitions
  switch (spots_fsm.state)
  {
  case FSMState::INIT:
    Serial.print("INIT");
    if (currDist > 20.0 && currDist < 400.0) {
      spots_fsm.new_state = FSMState::SWITCH;
      candidate_d_code = evalute_sensor_reading(currDist);
    }
    break;
  
  case FSMState::IDLE:
    Serial.print("IDLE");
    if (last_d_code != current_d_code) {
      spots_fsm.new_state = FSMState::PROBING; 
      candidate_d_code = last_d_code;
    }
    break;

  case FSMState::PROBING:
    Serial.print("PROBING");
    if (last_d_code != candidate_d_code) {spots_fsm.new_state = FSMState::IDLE;}
    else if (spots_fsm.cis >= cycles_threshold_4switch) {spots_fsm.new_state = FSMState::SWITCH;}
    break;

  case FSMState::SWITCH:
    Serial.print("SWITCH");
    spots_fsm.new_state = FSMState::IDLE;
    break;
  }

  // 2. State Transition
  set_state(spots_fsm, spots_fsm.new_state);

  Serial.print(" | NEW State = ");

  // 3. Perform Actions
  switch (spots_fsm.state)
  {
  case FSMState::INIT:
    Serial.println("INIT");
    break;
  
  case FSMState::IDLE:
    Serial.println("IDLE");
    break;

  case FSMState::PROBING:
    Serial.println("PROBING");
    break;

  case FSMState::SWITCH:
    Serial.println("SWITCH");
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

  WAKEUP_TIME = millis();
  Serial.begin(BAUDRATE);
  
  Serial.println(">>> WAKING UP...");

  //pinMode(ledPin, OUTPUT);
  pinMode(trigPin, OUTPUT);           //Envia um impulso
  pinMode(echoPin, INPUT);            //Recebe o impulso refletido
  digitalWrite(trigPin, LOW);

  // MAIN CODE RUNS BELOW (After that, the ESP32 will enter Deep Sleep Mode) ------------------

  int N_SAMPLES = 5;

  if (spots_fsm.state == FSMState::INIT){
    N_SAMPLES = 10;    
  }

  currDistance = compute_latest_distance(N_SAMPLES);

  Serial.print("Current distance (cm): ");
  Serial.println(currDistance);
  
  if (currDistance != -1){
    run_fsm_logic(currDistance);
  } else {
    Serial.println("<!> ERRO NA LEITURA - FSM não executada <!>");
  }

  if (SEND_NEW_STATE && sendReading()){
    SEND_NEW_STATE = false;
  }
  
  // Checks for distance requests every few cycles
  if (current_cycle_count - LAST_REQ_SENT_CYCLE >= NO_SENT_CYCLE_THRESHOLD){
    if (sendDistance(currDistance)) LAST_REQ_SENT_CYCLE = current_cycle_count;
  }
  
  go_to_deep_sleep();

}

void loop () {
 

}
