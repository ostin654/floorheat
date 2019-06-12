#include <QuadDisplay2.h>
#include <Wire.h>
#include <TroykaMQ.h>
#include <OneWire.h>
#include <GyverFilters.h>
#include <GyverTimer.h>

#define PIN_MQ5         A3
#define PIN_MQ5_HEATER  11
#define SERIAL_PIN 5
#define DS_PIN 10
#define PRESSURE_PIN A2
#define RELAY_PIN 12
#define QD_PIN 8
#define PING_PIN 7
#define GAS_FAILURE_PIN 9

#define GAS_FAILURE_LIMIT 30

#define I2C_SLAVE_ADDRESS 0x18

#define MY_UPTIME             0x99
#define REGISTER_LPG          0xa0
#define REGISTER_METHANE      0xa1
#define REGISTER_PRESSURE     0xa2
#define REGISTER_WELL_UPTIME  0xb9
#define REGISTER_WATER_LEVEL  0xb0
#define REGISTER_CUR_TEMP     0xc0
#define REGISTER_TARGET_TEMP  0xc1
#define REGISTER_CUR_STATE    0xc2
#define REGISTER_TARGET_STATE 0xc3
#define REGISTER_STATE_OFF    0xd0
#define REGISTER_STATE_AUTO   0xd1
#define REGISTER_GH_UPTIME    0xe9
#define REGISTER_GH_SOIL_HUM  0xe0
#define REGISTER_GH_WAT_PRESS 0xe1
#define REGISTER_GH_AIR_TEM   0xe2
#define REGISTER_GH_RELAY     0xe3
#define REGISTER_GH_DOOR      0xe4

//#define DEBUG
//#define DEBUG_I2C
//#define DEBUG_MB

#define MAX_BUFFER 64
#define T35 35

#define TELEGRAM_ADDR 0
#define TELEGRAM_FUNC 1
#define TELEGRAM_BYTE 2
#define TELEGRAM_ERRO 3
#define TELEGRAM_BYHI 4
#define TELEGRAM_BYLO 5
#define TELEGRAM_CRHI 6
#define TELEGRAM_CRLO 7

uint8_t au8Buffer[MAX_BUFFER];
uint8_t address = 0x00;
uint8_t function = 0x00;
uint8_t maxBytes = 0;
uint8_t errCode = 0;

MQ5 mq5(PIN_MQ5, PIN_MQ5_HEATER);
OneWire ds(DS_PIN);
QuadDisplay qd(QD_PIN);
GKalman analogFilter(40, 0.5);
GTimer_ms modbusTimer(2000);
GTimer_ms analogTimer(4000);
GTimer_ms displayTimer(1000);
GTimer_ms ds18b20Timer(4000);

struct DataPacket {
  unsigned long my_uptime = 0;                 // 0x99
  unsigned long lpg = 0;                       // 0xa0
  unsigned long methane = 0;                   // 0xa1
  unsigned long pressure = 0;                  // 0xa2
  unsigned long well_uptime = 0;               // 0xb9
  unsigned long water_level = 0;               // 0xb0
  unsigned long current_floor_temperature = 0; // 0xc0
  unsigned long target_floor_temperature = 10; // 0xc1
  unsigned long current_floor_state = 0;       // 0xc2
  unsigned long target_floor_state = 0;        // 0xc3
  unsigned long greenhouse_uptime = 0;         // 0xe9
  unsigned long greenhouse_soil_hum = 0;       // 0xe0
  unsigned long greenhouse_water_press = 0;    // 0xe1
  unsigned long greenhouse_air_temp = 0;       // 0xe2
  unsigned long greenhouse_relay_state = 0;    // 0xe3
  unsigned long greenhouse_door_state = 0;     // 0xe4
};

DataPacket packet;

// i2c
byte reg;
byte req = 0;
unsigned long to_send = 0;

// DS18B20
void dsMesure()
{
  ds.reset();
  ds.write(0xCC);
  ds.write(0x44, 1);
}

unsigned long dsGetTemperatureRaw()
{
  static unsigned long temperature = 0;
  if (ds18b20Timer.isReady())
  {
    ds.reset();
    ds.write(0xCC);
    ds.write(0xBE);

    byte data[2];
    data[0] = ds.read();
    data[1] = ds.read();
    temperature = (data[1] << 8) | data[0];

    dsMesure();
  }

  return temperature;
}

void setup()
{  
  pinMode(SERIAL_PIN, OUTPUT);
  digitalWrite(SERIAL_PIN, LOW);

  pinMode(GAS_FAILURE_PIN, OUTPUT);
  digitalWrite(GAS_FAILURE_PIN, LOW);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  analogReference(INTERNAL);

  dsMesure();

  qd.begin();
  qd.displayInt(0);

  Wire.begin(I2C_SLAVE_ADDRESS);
  delay(1000);
  Wire.onReceive(processMessage);
  Wire.onRequest(requestEvent);

  #ifdef DEBUG
  Serial.begin(9600);
  while (!Serial);
  #endif

  Serial1.begin(38400);
  while (!Serial1);

  mq5.heaterPwrHigh();
}
 
void loop()
{
  static bool device = false;

  // modbus send
  if (modbusTimer.isReady()) {
    uint8_t len = 0;
    if (device) {
      len = ReadInputRegisters(0x0A, 1, 2);
    } else {
      len = ReadInputRegisters(0x0B, 1, 6);
    }
    SendBuffer(len);
    device = !device;
  }

  // modbus receive
  if (WaitRespose()) {
    if (address == 0x0A) {
      packet.well_uptime = word(au8Buffer[3], au8Buffer[4]);
      packet.water_level = word(au8Buffer[5], au8Buffer[6]);
    } if (address == 0x0B) {
      packet.greenhouse_uptime = word(au8Buffer[3], au8Buffer[4]);
      packet.greenhouse_soil_hum = word(au8Buffer[5], au8Buffer[6]);
      packet.greenhouse_water_press = word(au8Buffer[7], au8Buffer[8]);
      packet.greenhouse_air_temp = word(au8Buffer[9], au8Buffer[10]);
      packet.greenhouse_relay_state = word(au8Buffer[11], au8Buffer[12]);
      packet.greenhouse_door_state = word(au8Buffer[13], au8Buffer[14]);
    }
  }

  if (!mq5.isCalibrated() && mq5.heatingCompleted()) {
    mq5.calibrate();
  }

  if (analogTimer.isReady()) {
    if (mq5.isCalibrated() && mq5.heatingCompleted()) {
      packet.lpg = mq5.readLPG();
      packet.methane = mq5.readMethane();
    }
    packet.pressure = analogRead(PRESSURE_PIN);
    packet.pressure = analogFilter.filtered(packet.pressure);
    packet.current_floor_temperature = dsGetTemperatureRaw();
  }

  if (packet.lpg > GAS_FAILURE_LIMIT || packet.methane > GAS_FAILURE_LIMIT) {
    digitalWrite(GAS_FAILURE_PIN, HIGH);
  } else {
    digitalWrite(GAS_FAILURE_PIN, LOW);
  }

  if (packet.target_floor_state == 0) {
    digitalWrite(RELAY_PIN, LOW);
    packet.current_floor_state = 0;
  } else {
    if (packet.current_floor_temperature * 0.0625 < packet.target_floor_temperature) {
      digitalWrite(RELAY_PIN, HIGH);
      packet.current_floor_state = 1;
    }
    if (packet.current_floor_temperature * 0.0625 > packet.target_floor_temperature + 3) {
      digitalWrite(RELAY_PIN, LOW);
      packet.current_floor_state = 0;
    }
  }

  if (displayTimer.isReady()) {
    if (millis() % 4000 < 2000) {
       qd.displayFloat((float)(packet.current_floor_temperature * 0.0625), 1);
    } else {
      if (packet.target_floor_state == 0) {
        qd.displayDigits(QD_NONE, QD_o, QD_f, QD_f); 
      } else {
        qd.displayInt(packet.target_floor_temperature);
      }
    }

    #ifdef DEBUG
    Serial.print("UP ");
    Serial.print(packet.my_uptime);
    Serial.print(" LPG ");
    Serial.print(packet.lpg);
    Serial.print(" ME ");
    Serial.print(packet.methane);
    Serial.print(" PR ");
    Serial.print(packet.pressure);
    Serial.print(" UP ");
    Serial.print(packet.well_uptime);
    Serial.print(" WA ");
    Serial.print(packet.water_level);
    Serial.print(" TE ");
    Serial.print(packet.current_floor_temperature);
    Serial.print(" TA ");
    Serial.print(packet.target_floor_temperature);
    Serial.print(" ST ");
    Serial.print(packet.current_floor_state);
    Serial.print(" TA ");
    Serial.print(packet.target_floor_state);
    Serial.print(" UP ");
    Serial.print(packet.greenhouse_uptime);
    Serial.print(" SO ");
    Serial.print(packet.greenhouse_soil_hum);
    Serial.print(" PR ");
    Serial.print(packet.greenhouse_water_press);
    Serial.print(" TE ");
    Serial.print(packet.greenhouse_air_temp);
    Serial.print(" RE ");
    Serial.print(packet.greenhouse_relay_state);
    Serial.print(" DO ");
    Serial.print(packet.greenhouse_door_state);
    Serial.println();
    #endif
  }
}

void processMessage(int n) {
  byte data = 0;
  data = Wire.read();

  #ifdef DEBUG_I2C
  Serial.print("i2c process ");
  Serial.println(data);
  #endif

  switch (data) {
    case MY_UPTIME:             //0x99
    case REGISTER_LPG:          //0xa0
    case REGISTER_METHANE:      //0xa1
    case REGISTER_PRESSURE:     //0xa2
    case REGISTER_WELL_UPTIME:  //0xb9
    case REGISTER_WATER_LEVEL:  //0xb0
    case REGISTER_CUR_TEMP:     //0xc0
    case REGISTER_TARGET_TEMP:  //0xc1
    case REGISTER_CUR_STATE:    //0xc2
    case REGISTER_TARGET_STATE: //0xc3
    case REGISTER_GH_UPTIME:    //0xe9
    case REGISTER_GH_SOIL_HUM:  //0xe0
    case REGISTER_GH_WAT_PRESS: //0xe1
    case REGISTER_GH_AIR_TEM:   //0xe2
    case REGISTER_GH_RELAY:     //0xe3
    case REGISTER_GH_DOOR:      //0xe4
      reg = data;
      #ifdef DEBUG_I2C
      Serial.print("Reg set ");
      Serial.println(reg);
      #endif
      break;
    case REGISTER_STATE_OFF:
      packet.target_floor_state = 0;
      #ifdef DEBUG_I2C
      Serial.println("State off");
      #endif
      break;
    case REGISTER_STATE_AUTO:
      packet.target_floor_state = 1;
      #ifdef DEBUG_I2C
      Serial.println("State on");
      #endif
      break;
    case 0xff: // exclude i2cdetect
      #ifdef DEBUG_I2C
      Serial.println("Skip");
      #endif
      break;
    default:
      if (10 <= data && data <= 38) {
        packet.target_floor_temperature = data;
      }
      #ifdef DEBUG_I2C
      Serial.print("Data ");
      Serial.println(data);
      Serial.print("Temp set ");
      Serial.println(packet.target_floor_temperature);
      #endif
      break;
  }

  req = 0;
}

void requestEvent() {
  #ifdef DEBUG_I2C
  Serial.print("i2c request ");
  Serial.print(reg);
  Serial.print(" ");
  Serial.print(req);
  #endif

  if (req == 0) {
    switch (reg) {
      case MY_UPTIME:
        to_send = packet.my_uptime;
        break;
      case REGISTER_LPG:
        to_send = packet.lpg;
        break;
      case REGISTER_METHANE:
        to_send = packet.methane;
        break;
      case REGISTER_PRESSURE:
        to_send = packet.pressure;
        break;
      case REGISTER_WELL_UPTIME:
        to_send = packet.well_uptime;
        break;
      case REGISTER_WATER_LEVEL:
        to_send = packet.water_level;
        break;
      case REGISTER_CUR_TEMP:
        to_send = packet.current_floor_temperature;
        break;
      case REGISTER_TARGET_TEMP:
        to_send = packet.target_floor_temperature;
        break;
      case REGISTER_CUR_STATE:
        to_send = packet.current_floor_state;
        break;
      case REGISTER_TARGET_STATE:
        to_send = packet.target_floor_state;
        break;
      case REGISTER_GH_UPTIME:
        to_send = packet.greenhouse_uptime;
        break;
      case REGISTER_GH_SOIL_HUM:
        to_send = packet.greenhouse_soil_hum;
        break;
      case REGISTER_GH_WAT_PRESS:
        to_send = packet.greenhouse_water_press;
        break;
      case REGISTER_GH_AIR_TEM:
        to_send = packet.greenhouse_air_temp;
        break;
      case REGISTER_GH_RELAY:
        to_send = packet.greenhouse_relay_state;
        break;
      case REGISTER_GH_DOOR:
        to_send = packet.greenhouse_door_state;
        break;      
      default:
        to_send = packet.my_uptime;
        break;
    }
  }

  byte b = 0;

  #ifdef DEBUG_I2C
  Serial.print(" ");
  Serial.println((byte)(to_send | b));
  #endif

  Wire.write((byte)(to_send | b));
  to_send = to_send >> 8;
  req = (req + 1) % 4;
}


uint8_t ReadInputRegisters(uint8_t address, uint16_t firstReg, uint16_t regNumber) {
  uint8_t i = 0;
  uint16_t crc = 0;

  au8Buffer[i++] = address; // device address
  au8Buffer[i++] = 0x04; // function code
  au8Buffer[i++] = highByte(firstReg);
  au8Buffer[i++] = lowByte(firstReg);
  au8Buffer[i++] = highByte(regNumber);
  au8Buffer[i++] = lowByte(regNumber);

  // append CRC to message
  crc = CalculateCRC(i);
  au8Buffer[i++] = crc >> 8;
  au8Buffer[i++] = crc & 0x00ff;

  return i;
}

void SendBuffer(uint8_t len) {
  delay(20);
  digitalWrite(SERIAL_PIN, HIGH);
  delay(20);
  #ifdef DEBUG_MB
  for (uint8_t j=0; j<len;j++) {
    Serial.print(au8Buffer[j]);
    Serial.print(" ");
  }
  Serial.println();
  #endif;
  Serial1.write(au8Buffer, len);
  delay(20);
}

bool WaitRespose() {
  uint8_t len;
  digitalWrite(SERIAL_PIN, LOW);
  len = ReceiveTelegram();
  if (len > 0 && errCode == 0) {
    return true;
  } else {
    return false;
  }
}

uint8_t ReceiveTelegram() {
  static uint8_t telegramStep = 0;
  static uint8_t i = 0;
  static uint32_t telegramStart = 0;
  static uint8_t byteCnt = 0;

  if (millis() - telegramStart > T35) {
    telegramStep = TELEGRAM_ADDR;
    byteCnt = i = 0;
    errCode = 0;
  }

  if (Serial1.available()) {
    byte c = Serial1.read();
    telegramStart = millis();
    #ifdef DEBUG_MB
    Serial.print(c);
    Serial.print(" ");
    Serial.print(byteCnt);
    Serial.print(" ");
    Serial.println(telegramStep);
    #endif
    switch(telegramStep) {
      case TELEGRAM_ADDR:
        address = au8Buffer[i++] = c;
        telegramStep = TELEGRAM_FUNC;
        return 0;
        break;
      case TELEGRAM_FUNC:
        function = au8Buffer[i++] = c;
        telegramStep = TELEGRAM_BYTE;
        if (function >= 0x80) {
          telegramStep = TELEGRAM_ERRO;
        }
        return 0;
        break;
      case TELEGRAM_ERRO:
        errCode = au8Buffer[i++] = c;
        telegramStep = TELEGRAM_CRHI;
        return 0;
        break;
      case TELEGRAM_BYTE:
        maxBytes = au8Buffer[i++] = c;
        if (byteCnt >= maxBytes) {
          telegramStep = TELEGRAM_CRHI;
        } else {
          telegramStep = TELEGRAM_BYHI;
        }
        return 0;
        break;
      case TELEGRAM_BYHI:
        au8Buffer[i++] = c;
        byteCnt++;
        if (byteCnt >= maxBytes) {
          telegramStep = TELEGRAM_CRHI;
        } else {
          telegramStep = TELEGRAM_BYLO;
        }
        return 0;
        break;
      case TELEGRAM_BYLO:
        au8Buffer[i++] = c;
        byteCnt++;
        if (byteCnt >= maxBytes) {
          telegramStep = TELEGRAM_CRHI;
        } else {
          telegramStep = TELEGRAM_BYHI;
        }
        return 0;
        break;
      case TELEGRAM_CRHI:
        au8Buffer[i+1] = c;
        telegramStep = TELEGRAM_CRLO;
        return 0;
        break;
      case TELEGRAM_CRLO:
        {
          au8Buffer[i+2] = c;
          telegramStep = TELEGRAM_ADDR;
          uint8_t len = i;
          uint16_t crc = 0;
          i = 0;

          crc = au8Buffer[len+1] << 8;
          crc |= au8Buffer[len+2];
          if (crc != CalculateCRC(len)) {
            Serial.print(" WRONG CRC ");
            Serial.print(word(au8Buffer[len+1], au8Buffer[len+2]), DEC);
            Serial.print(CalculateCRC(len), DEC);
            return 0;
          }
          return len;
        }
        break;
      default:
        telegramStep = TELEGRAM_ADDR;
        i = 0;
        return 0;
        break;
    }
  } else {
    return 0;
  }
}

uint16_t CalculateCRC(uint8_t u8length) {
    unsigned int temp, temp2, flag;
    temp = 0xFFFF;
    for (unsigned char i = 0; i < u8length; i++)
    {
        temp = temp ^ au8Buffer[i];
        for (unsigned char j = 1; j <= 8; j++)
        {
            flag = temp & 0x0001;
            temp >>=1;
            if (flag)
                temp ^= 0xA001;
        }
    }
    // Reverse byte order.
    temp2 = temp >> 8;
    temp = (temp << 8) | temp2;
    temp &= 0xFFFF;
    // the returned value is already swapped
    // crcLo byte is first & crcHi byte is last
    return temp;
}
