#include <max6675.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SD.h>

//pin definition
#define FAN A0
#define H70 A1
#define H50 A2
#define H_CAP A3
#define SD_cs 2
#define H50_p 3
#define R_BTN 4
#define H70_p 5
#define H_CAP_p 6
#define THRM_B1 7
#define THRM_CAP 8
#define THRM_B2 9
#define L_BTN 10
#define MISO 12
#define MOSI 11
#define CLK 13

//control variables
int H70_pwm = 0;
int H50_pwm = 0;
int H_CAP_pwm = 0;

bool H70_act = false;
bool H50_act = false;
bool H_CAP_act = false;
bool FAN_act = false;

bool AZ_5 = false;
String AZ_5_info = "";

float target_temp_block = 0;
float target_temp_cap = 0;
bool heat_act = false;

float temp_cap = 0;
float temp_body_1 = 0;
float temp_body_2 = 0;

long temp_timeout = 600000;
long temp_tim_tim = 0;
bool set_timeout = false;
bool AZ_tim=false;

//default program states
bool force_end = false;
bool program_start = false;
int program_step = 0;
int abs_program_step = 0;
unsigned long step_start_time = 0;
bool holding = false;

//temperature cycling program
const int program_length = 3;
float program_block_targets[program_length] = {95.0, 60.0, 72.0};  
float program_cap_targets[program_length]   = {110, 110, 110};  
unsigned long hold_times[program_length]    = {30000, 30000, 45000}; 

int end_hold = 60000;

int cycles = 2;

bool program_end_phase = false;
unsigned long end_hold_start = 0;
bool cooling_started = false;


//derivative calculation
const int RATE_HISTORY_SIZE = 10;

float rate_cap_history[RATE_HISTORY_SIZE] = {0};
float rate_body_history[RATE_HISTORY_SIZE] = {0};
int rate_index = 0;

float prev_temp_cap = 0.0;
float prev_temp_body_1 = 0.0;

float avg_rate_cap = 0.0;
float avg_rate_body = 0.0;

unsigned long last_temp_update = 0;



//lcd initialization
LiquidCrystal_I2C lcd(0x27, 20, 4); 

MAX6675 thermo_cap(CLK, THRM_CAP, MISO);
MAX6675 thermo_body_1(CLK, THRM_B1, MISO);
MAX6675 thermo_body_2(CLK, THRM_B2, MISO);

bool serial = false;
bool SD_b = true;

//writes the header for the log file
void writeLogHeader() {
  if (SD_b) {
    if (SD.begin(2)) {

      Serial.println("SD card connected!");
      lcd.print("SD: connected");

      if (SD.exists("log.csv")) {
        SD.remove("log.csv"); // Delete the existing file
      }

      File dataFile = SD.open("log.csv", FILE_WRITE);
      if (dataFile && dataFile.size() == 0) {  // Only write if file is empty
        dataFile.println(
          "millis,"
          "block_temp_1,"
          "block_temp_2,"
          "block_temp_mean,"
          "temp_cap,"
          "block_gradient,"
          "cap_gradient,"
          "l_btn,"
          "r_btn,"
          "H70_pwm,"
          "H50_pwm,"
          "H_CAP_pwm,"
          "H70_act,"
          "H50_act,"
          "H_CAP_act,"
          "FAN_act,"
          "AZ_5,"
          "target_temp_block,"
          "target_temp_cap,"
          "heat_act"
        );
        dataFile.close();
      }
      digitalWrite(SD_cs, HIGH); 
      SD.end();  // Free SPI bus
    } else {
      Serial.println("SD init failed!");
      lcd.setCursor(0, 3);
      lcd.print("SD CRITICAL FAILURE"); //CANNOT WORK WITHOUT SD PRESENT AS THERE ARE SPI CONFLICTS, IDK WHY AND I SPENT TOO MUCH TIME ON IT ALREADY
      SD.end();
      while(true){;}
      digitalWrite(SD_cs, HIGH); 
    }
  }
}


void setup() {

  //start serial comm
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(100);

  //initialize lcd
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Connecting to serial");
  lcd.setCursor(0, 1);

  //pin setup
  pinMode(FAN, OUTPUT);
  pinMode(H70, OUTPUT);
  pinMode(H50, OUTPUT);
  pinMode(H_CAP, OUTPUT);
  pinMode(H50_p, OUTPUT);
  pinMode(H70_p, OUTPUT);
  pinMode(H_CAP_p, OUTPUT);

  pinMode(R_BTN, INPUT_PULLUP);
  pinMode(L_BTN, INPUT_PULLUP);

  //try to connect to serial with handshake
  for(int i = 0; i<20;i++){
    Serial.println("syn");
    String response = "";
    response = Serial.readStringUntil('\n');
    if(response.equals("syn ack")){
      serial = true; 
      Serial.println("ack");
      break;
    }
    if(response.equals("syn ack no_sd")){
      serial = true; 
      SD_b = false;
      Serial.println("ack");
      break;
    }
    lcd.print((char)255);
    delay(700);
    if(digitalRead(R_BTN)==LOW || digitalRead(L_BTN)==LOW){break;}
  }

  lcd.clear();
  
  lcd.setCursor(0, 0);
  lcd.print("Thermocycler init...");
  lcd.setCursor(0, 1);
  lcd.print("Serial: ");
  serial ? lcd.print("true") : lcd.print("false");
  lcd.setCursor(0, 2);
  
  if(SD_b){

    writeLogHeader();

  }
  delay(2000);
  lcd.setCursor(0, 0);
  lcd.print("System ready        ");
  lcd.setCursor(0, 3);
  lcd.print("@Ale-G 2025  V1.1.0");

  
  delay(5000);
  lcd.clear();

}

bool l_btn, r_btn;

long last_update = 10000000;

bool arrived_at_temp = false;

//heating control function
void update_values(){

  float d_t_block = target_temp_block-temp_body_1;
  float d_t_cap = target_temp_cap-temp_cap;

  FAN_act = d_t_block<-2 && heat_act;

  H70_act = d_t_block>-2 && heat_act;
  H50_act = d_t_block>-2 && heat_act;

  H_CAP_act = d_t_cap>-2 && heat_act;

  if(abs(d_t_block)<1){

    arrived_at_temp=true;

  }
  else{

    arrived_at_temp=false;

  }

  if(d_t_block>1.5){

    H70_pwm=255;
    H50_pwm=255;

  }
  else if(d_t_block>0){

    H70_pwm=100;
    H50_pwm=100;


  }
  else{

    H70_pwm=0;
    H50_pwm=0;

  }
  
  if(d_t_cap>1.5){

    H_CAP_pwm=255;

  }
  else if(d_t_cap>0){

    H_CAP_pwm=100;

  }
  else{

    H_CAP_pwm=0;

  }

}

long t_p_r = 0;
long t_p_l = 0;

long last_millis = 0;

//converts serial commands in variable changes
void handleCommand(String cmd) {
  int equalsIndex = cmd.indexOf('=');
  if (equalsIndex == -1) return;

  String var = cmd.substring(0, equalsIndex);
  String val = cmd.substring(equalsIndex + 1);

  var.trim();
  val.trim();

  // integer variables
  if (var == "H_pwm") {H70_pwm = val.toInt(); H50_pwm = val.toInt();}
  else if (var == "H_CAP_pwm") H_CAP_pwm = val.toInt();

  // float variables
  else if (var == "target_block_temp") target_temp_block = val.toFloat();
  else if (var == "target_cap_temp") target_temp_cap = val.toFloat();

  // boolean variables
  else if (var == "H_act") {H70_act = parseBool(val);H50_act = parseBool(val);}
  else if (var == "H_CAP_act") H_CAP_act = parseBool(val);
  else if (var == "FAN_act") FAN_act = parseBool(val);
  else if (var == "AZ_5") {AZ_5 = parseBool(val);AZ_5_info="REMOTE SHUTDOWN";}
  else if (var == "heat_act") heat_act = parseBool(val);
  else {Serial.print("Unrecognized variable");return;}
  // confirm output
  Serial.print("Set ");
  Serial.print(var);
  Serial.print(" to ");
  Serial.println(val);
}

bool parseBool(String val) {
  val.toLowerCase();
  return (val == "1" || val == "true");
}

//sd logging
void logToSD() {
  if (SD_b) {
    if (SD.begin(2)) {
      File dataFile = SD.open("log.csv", FILE_WRITE);
      if (dataFile) {
        float block_temp = (temp_body_1 + temp_body_2) / 2.0;

        dataFile.print(millis()); dataFile.print(",");
        dataFile.print(temp_body_1, 2); dataFile.print(",");
        dataFile.print(temp_body_2, 2); dataFile.print(",");
        dataFile.print(block_temp, 2); dataFile.print(",");
        dataFile.print(temp_cap); dataFile.print(",");
        dataFile.print(avg_rate_body); dataFile.print(",");
        dataFile.print(avg_rate_cap); dataFile.print(",");
        dataFile.print(l_btn); dataFile.print(",");
        dataFile.print(r_btn); dataFile.print(",");

        dataFile.print(H70_pwm); dataFile.print(",");
        dataFile.print(H50_pwm); dataFile.print(",");
        dataFile.print(H_CAP_pwm); dataFile.print(",");

        dataFile.print(H70_act ? "1" : "0"); dataFile.print(",");
        dataFile.print(H50_act ? "1" : "0"); dataFile.print(",");
        dataFile.print(H_CAP_act ? "1" : "0"); dataFile.print(",");
        dataFile.print(FAN_act ? "1" : "0"); dataFile.print(",");

        dataFile.print(AZ_5 ? "1" : "0"); dataFile.print(",");
        dataFile.print(target_temp_block, 2); dataFile.print(",");
        dataFile.print(target_temp_cap, 2); dataFile.print(",");
        dataFile.print(heat_act ? "1" : "0");

        dataFile.println(); // End the CSV row
        dataFile.close();
      } else {
        Serial.println("SD card failed!");
        lcd.setCursor(0, 3);
        lcd.print("SD Fail");
      }
      digitalWrite(SD_cs, HIGH); 
      SD.end();  // Free SPI bus
    } else {
      Serial.println("SD init failed!");
      lcd.setCursor(0, 3);
      lcd.print("SD Fail");
      SD.end();
      digitalWrite(SD_cs, HIGH); 
    }
  }
}

//calculate derivative
void updateTemperatureRate() {
  unsigned long current_time = millis();
  float dt = (current_time - last_temp_update) / 1000.0; 

  if (dt > 0.5) {  //update every 0.5s or more
    float rate_cap = (temp_cap - prev_temp_cap) / dt;
    float rate_body = (temp_body_1 - prev_temp_body_1) / dt;

    //save for next delta
    prev_temp_cap = temp_cap;
    prev_temp_body_1 = temp_body_1;
    last_temp_update = current_time;

    //store in circular buffer
    rate_cap_history[rate_index] = rate_cap;
    rate_body_history[rate_index] = rate_body;
    rate_index = (rate_index + 1) % RATE_HISTORY_SIZE;

    //compute averages
    float sum_cap = 0;
    float sum_body = 0;
    for (int i = 0; i < RATE_HISTORY_SIZE; i++) {
      sum_cap += rate_cap_history[i];
      sum_body += rate_body_history[i];
    }

    avg_rate_cap = sum_cap / RATE_HISTORY_SIZE;
    avg_rate_body = sum_body / RATE_HISTORY_SIZE;

  }
}

int scene = 0;
bool update_screen = false;

bool r_btn_stk = false;
bool l_btn_stk = false;

long r_btn_debounce = 0;
long l_btn_debounce = 0;

long cooling_phase = 0;

int pointer = 0;
int redundancy_score = 0;
long red_mismatch_timer = 0;

void loop() {

  t_p_r+=millis()-last_millis;
  t_p_l+=millis()-last_millis;

  last_millis=millis();

  if(digitalRead(L_BTN)==LOW){l_btn=true; l_btn_debounce=millis();}else if (millis()-l_btn_debounce>10){l_btn=false;t_p_l = 0;l_btn_stk=false;}
  if(digitalRead(R_BTN)==LOW){r_btn=true; r_btn_debounce=millis();}else if (millis()-r_btn_debounce>10){r_btn=false;t_p_r = 0;r_btn_stk=false;}
  
  if(t_p_r>4000 && t_p_l>4000){

    AZ_5=true;
    AZ_5_info="MANUAL SHUTDOWN";

  }

  bool time_since_upd = (millis()-last_update)>500;

  if(time_since_upd){

    //needed to resolve spi bus conflict
    MAX6675 thermo_cap(CLK, THRM_CAP, MISO);
    MAX6675 thermo_body_1(CLK, THRM_B1, MISO);
    MAX6675 thermo_body_2(CLK, THRM_B2, MISO);

    temp_cap = thermo_cap.readCelsius();
    temp_body_1 = thermo_body_1.readCelsius();
    temp_body_2 = thermo_body_2.readCelsius();

    logToSD();

    updateTemperatureRate();  

    last_update=millis();

  }

  //screen update function
  if(time_since_upd || update_screen){

    if(update_screen){

      lcd.clear();
      update_screen=false;

    }
    
    if(t_p_r>1000 && t_p_l>1000){

      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("   AZ-5 ACTIVATING  ");
      lcd.setCursor(0,2);
      lcd.print("        ");
      lcd.print(String(4-(t_p_r+t_p_l)/2000));
      lcd.print(".00");


    }
    else if(scene == 0){

      lcd.setCursor(0, 0);
      lcd.print("B:");
      lcd.print(temp_body_1, 2);
      lcd.print((char)223);
      lcd.print("C>");
      lcd.print(target_temp_block, 2);
      lcd.print((char)223);
      lcd.print("C ");

      lcd.setCursor(0, 1);
      lcd.print("C:");
      lcd.print(temp_cap, 2);
      lcd.print((char)223);
      lcd.print("C>");
      lcd.print(target_temp_cap, 2);
      lcd.print((char)223);
      lcd.print("C ");

      lcd.setCursor(0, 2);
      lcd.print("Fan:");
      lcd.print(FAN_act ? "On " : "Off");
      lcd.print(" Pwm:");
      lcd.print(H50_pwm);
      lcd.print("   "); // clear trailing chars if needed

      lcd.setCursor(0, 3);
      lcd.print(" Settings     Start ");


    }
    else if(scene == 2){

      lcd.setCursor(0, 0);
      lcd.print("B:");
      lcd.print(temp_body_1, 2);
      lcd.print((char)223);
      lcd.print("C>");
      lcd.print(target_temp_block, 2);
      lcd.print((char)223);
      lcd.print("C ");

      lcd.setCursor(0, 1);
      lcd.print("C:");
      lcd.print(temp_cap, 2);
      lcd.print((char)223);
      lcd.print("C>");
      lcd.print(target_temp_cap, 2);
      lcd.print((char)223);
      lcd.print("C ");

      lcd.setCursor(0, 2);
      lcd.print("F:");
      lcd.print(FAN_act ? "T" : "F");
      lcd.print(" P:");
      lcd.print(H50_pwm);

      H50_pwm>9 ?  lcd.print("") : lcd.print(" ");
      H50_pwm>99 ?  lcd.print("") : lcd.print(" ");
      if(holding && !program_end_phase){
        lcd.print(" T: ");
        lcd.print(String((millis() - step_start_time)/1000) +">"+ String((hold_times[program_step])/1000));
      }
      else if(program_end_phase && !cooling_started){
        lcd.print(" T: ");
        lcd.print(String((millis() - end_hold_start)/1000) +">"+ String((end_hold)/1000));
      }
      else{
        lcd.print("           ");
      }

      lcd.setCursor(0, 3);
      if (program_start && !program_end_phase) {
        lcd.print("Step ");
        lcd.print(program_step + 1);
        lcd.print("/");
        lcd.print(program_length);
        lcd.print(" Cycle: ");
        lcd.print(floor(abs_program_step/program_length)+1,0);
        lcd.print("/"+String(cycles));

      } else if (program_end_phase && !cooling_started) {
        lcd.print("Ending phase...     ");
      } else if (cooling_started) {
        lcd.print("Cooling Down     ");
      } else {
        lcd.print(heat_act ? "System  ARMED" : "System SAFE");
      }


    }
    else if(scene==1){
      lcd.setCursor(0,0);
      lcd.print("Start program?");
      lcd.setCursor(0,2);
      lcd.print("    Yes       No    ");
    }
    else if(scene==3){
      lcd.setCursor(0,0);
      lcd.print("Ending hold?");
      lcd.setCursor(0,2);
      lcd.print("    Yes       No    ");
    }
    else if(scene==4){
      lcd.setCursor(0,0);
      (pointer == 0) ? lcd.print("> ") : lcd.print("");
      lcd.print("Cycles:");
      lcd.print(cycles);
      lcd.setCursor(0,1);
      (pointer == 1) ? lcd.print("> ") : lcd.print("");
      lcd.print("Extension:");
      lcd.print(String(hold_times[2]/1000)+"s");
      lcd.setCursor(0,2);
      (pointer == 2) ? lcd.print("> ") : lcd.print("");
      lcd.print("Save and exit");
      lcd.setCursor(0,3);
      lcd.print("  Change      Down  ");
    }

  }

  //button choice select
  if(scene==0 && r_btn==true && l_btn==false && r_btn_stk==false && !program_start){

    scene = 1;
    r_btn_stk=true;
    update_screen=true;

  }
  if(scene==0 && l_btn==true && r_btn==false && l_btn_stk==false && !program_start){

    scene = 4;
    l_btn_stk=true;
    update_screen=true;

  }
  if(scene==4){

    if(r_btn==true && l_btn==false && r_btn_stk==false && !program_start){
    
      pointer++;
      r_btn_stk=true;
      if(pointer>=3){pointer=0;};
      update_screen=true;

    }
    if(l_btn==true && r_btn==false && l_btn_stk==false && !program_start && pointer==0){

      cycles++;
      l_btn_stk=true;

    }
    if(t_p_l>2000 && r_btn==false && pointer==0){

      cycles=0;
      update_screen=true;

    }
    if(l_btn==true && r_btn==false && l_btn_stk==false && !program_start && pointer==1){

      hold_times[2] = hold_times[2]+15000;
      l_btn_stk=true;

    }
    if(t_p_l>2000 && r_btn==false && pointer==1){

      hold_times[2] = 30000;
      update_screen=true;

    }
    if(l_btn==true && r_btn==false && l_btn_stk==false && !program_start && pointer==2){

      scene=0;
      l_btn_stk=true;
      pointer=0;
      update_screen=true;

    }

  }

  if(scene==1 && r_btn==true && l_btn==false && r_btn_stk==false && !program_start){
    scene = 0;
    r_btn_stk=true;
    update_screen=true;

  }
  if(scene==1 && l_btn==true && r_btn==false && l_btn_stk==false && !program_start){
    scene = 2;
    l_btn_stk=true;
    update_screen=true;
    program_start=true;
    heat_act=true;
    abs_program_step = 0;

  }
  if(scene==2 && r_btn==true && l_btn==false && r_btn_stk==false && program_start){

    scene = 3;
    r_btn_stk=true;
    update_screen=true;

  }
  if(scene==3 && r_btn==true && l_btn==false && r_btn_stk==false && program_start){

    scene = 2;
    r_btn_stk=true;
    update_screen=true;

  }
  if(scene==3 && l_btn==true && r_btn==false && l_btn_stk==false && program_start){

    scene = 2;
    l_btn_stk=true;
    update_screen=true;
    force_end=true;

  }


  //program running
  if (program_start && !AZ_5) {

    if (!program_end_phase) {

      //set target temps for current step
      target_temp_block = program_block_targets[program_step];
      target_temp_cap = program_cap_targets[program_step];

      if(!set_timeout){

        temp_tim_tim=millis();
        set_timeout=true;

      }
      if(millis()-temp_tim_tim>temp_timeout){

        AZ_5=true;
        AZ_5_info="HEATING TIMEOUT";

      }

      update_values();

      //holding temps
      if (arrived_at_temp && !holding) {
        holding = true;
        step_start_time = millis();
        update_screen = true;
      }

      if (holding && (millis() - step_start_time >= hold_times[program_step])) {
        program_step++;
        abs_program_step++;
        holding = false;
        set_timeout=false;
        update_screen = true;
      }

      //end hold
      if ((program_step >= program_length && abs_program_step >= program_length*cycles)||force_end) {
        force_end=false;
        program_end_phase = true;
        end_hold_start = millis();
        target_temp_block = 72;  // Hold at 55C
        update_screen = true;
      }
      else if(program_step >= program_length){
        program_step = 0;
      }
      //failsafe
      if(program_step == program_length){
        program_step=0;
      }

    } else {
      
      if (!cooling_started && (millis() - end_hold_start >= end_hold)) {
        cooling_started = true;
        target_temp_block = 0;
        target_temp_cap = 0;
        cooling_phase = millis();
        update_screen=true;
      }
      else if ((millis()-cooling_phase >=180000 || temp_body_1<26) && cooling_started){

        heat_act=false;
        program_start=false;
        holding=false;
        program_step=0;
        abs_program_step=0;
        program_end_phase=false;
        cooling_started=false;
        update_screen=true;
        scene=0;

      }
    }
  }

  //sanity checks
  if(temp_body_1>130||temp_body_2>130||temp_cap>130){
    AZ_5=true;
    AZ_5_info="TEMP RUNAWAY";
  }
  else if (isnan(temp_body_1) || isnan(temp_body_2) || isnan(temp_cap) || temp_body_1==0 || temp_body_2==0 || temp_cap==0) {
    AZ_5 = true;
    AZ_5_info = "SENSOR FAULT";
  }
  if (abs(temp_body_1 - temp_body_2) > 15) {
      if (millis() - red_mismatch_timer > 1000) {
          redundancy_score++;
          red_mismatch_timer = millis();
      }
  } else {
      redundancy_score = 0;
  }
  if(redundancy_score>5){
    AZ_5 = true;
    AZ_5_info = "REDUNDANCY MISMATCH";
  }


  //scram function
  if(!AZ_5){

    if(heat_act){

      digitalWrite(FAN, FAN_act ? LOW : HIGH);
      digitalWrite(H70, H70_act ? LOW : HIGH);
      digitalWrite(H50, H50_act ? LOW : HIGH);
      digitalWrite(H_CAP, H_CAP_act ? LOW : HIGH);

      analogWrite(H70_p, H70_pwm);
      analogWrite(H50_p, H50_pwm);
      analogWrite(H_CAP_p, H_CAP_pwm);

    }
    else{

      digitalWrite(FAN, HIGH);
      digitalWrite(H70, HIGH);
      digitalWrite(H50, HIGH);
      digitalWrite(H_CAP, HIGH);

    }
    update_values();
    

  }
  else{

    heat_act = false;

    digitalWrite(FAN, LOW);
    digitalWrite(H70, HIGH);
    digitalWrite(H50, HIGH);
    digitalWrite(H_CAP, HIGH);

    Serial.println("AZ 5 ACTIVATED");
    Serial.println("MANUAL RESET NEEDED");

    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("AZ-5 ACTIVATED");
    lcd.setCursor(0,1);
    lcd.print("ALL COOLING ACTIVE");
    lcd.setCursor(0,2);
    lcd.print(AZ_5_info);
    lcd.setCursor(0,3);
    lcd.print("RESET TO CONTINUE...");

    if (SD_b) {

      if (SD.begin(2)) {
        
        File dataFile = SD.open("log.csv", FILE_WRITE);
        if (dataFile) {

          dataFile.print("AZ_5 ACTIVATED, REASON: ");dataFile.print(",");
          dataFile.print(AZ_5_info);
          dataFile.println(); // End the CSV row
          dataFile.close();

        }

        digitalWrite(SD_cs, HIGH); 
        SD.end();  // Free SPI bus

      } 
      else {

        SD.end();
        digitalWrite(SD_cs, HIGH); 

      }
    }
    //send limited diagnostics
    while(true){
      
      MAX6675 thermo_cap(CLK, THRM_CAP, MISO);
      MAX6675 thermo_body_1(CLK, THRM_B1, MISO);
      MAX6675 thermo_body_2(CLK, THRM_B2, MISO);

      temp_cap = thermo_cap.readCelsius();
      temp_body_1 = thermo_body_1.readCelsius();
      temp_body_2 = thermo_body_2.readCelsius();
      
      delay(200);

      String response = String("{") +
      "\"block_temperature\": " + String(temp_body_1, 2) + String(", ") +
      "\"cap_temperature\": " + String(temp_cap) + String(" ") +
      "}";

      Serial.println(response);

    }

  }

  //handle serial communication
  if(!serial){
    Serial.println("-------------");
    Serial.print("Cap: "); Serial.print(temp_cap); Serial.println("C");
    Serial.print("PWM_cap: "); Serial.print(H70_pwm); Serial.println("/255 ");

    Serial.print("Block 1: "); Serial.print(temp_body_1); Serial.println("C");
    Serial.print("Block 2: "); Serial.print(temp_body_2); Serial.println("C");
    Serial.print("PWM_block: "); Serial.print(H_CAP_pwm); Serial.println("/255 ");

    heat_act ? Serial.println("Heat_act: True") : Serial.println("Heat_act: False");
    Serial.println("-------------");
    delay(50);
  }
  else{

    String response = String("{") +
      "\"block_temperature\": " + String(temp_body_1, 2) + String(", ") +
      "\"target_block_temp\": " + String(target_temp_block, 2) + String(", ") +
      "\"block_gradient\": " + String(avg_rate_body , 2) + String(", ") +
      "\"cap_temperature\": " + String(temp_cap) + String(", ") +
      "\"target_cap_temp\": " + String(target_temp_cap, 2) + String(", ") +
      "\"cap_gradient\": " + String(avg_rate_cap, 2) + String(", ") +
      "\"redundant_temp\": " + String(temp_body_2, 2) + String(", ") +
      "\"buttons\": [" + String(l_btn) + ", " + String(r_btn) + "], " +

      "\"H_pwm\": " + String(H70_pwm) + ", " +
      "\"H_CAP_pwm\": " + String(H_CAP_pwm) + ", " +

      "\"H_act\": " + (H70_act ? "true" : "false") + ", " +
      "\"H_CAP_act\": " + (H_CAP_act ? "true" : "false") + ", " +
      "\"FAN_act\": " + (FAN_act ? "true" : "false") + ", " +

      "\"AZ_5\": " + (AZ_5 ? "true" : "false") + ", " +
      
      "\"heat_act\": " + (heat_act ? "true" : "false") +", " +
      "\"temp_reached\": " + (arrived_at_temp ? "true" : "false") +", " +
      "\"holding_temp\": " + (holding ? "true" : "false") +", " +
      "\"program_active\": " + (program_start ? "true" : "false") +", " +
      "\"timers\": [" + ((holding && !program_end_phase) ? String((millis() - step_start_time)/1000) : String("0")) +", " + String((hold_times[program_step])/1000) +"], " +
      "\"end_timers\": [" + ((program_end_phase && !cooling_started) ? String((millis() - end_hold_start)/1000) : String("0")) +", " + String((end_hold)/1000) +"] " +
    "}";


    Serial.println(response);

    //wait for input
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command.length() > 0) {
      Serial.println("ack");
      handleCommand(command);
    }

  }

}


