#include <avr/wdt.h>

#if !defined(__MK20DX256__) && !defined(__MK20DX128__) && !defined(__MKL26Z64__) // not Teensy 3.x/LC
#define I2C_NOSTOP false
#define I2C_STOP true
#endif

#if defined (__SAM3X8E__) // Arduino Due only
// GoPro camera already has pull-up resistors on the I2C bus inside. 
// Due's Wire lib, however, uses D20 and D21 as SDA and SCL respectively, which have pull-up resistors of 1k ohm, too. 
// Thus in order to avoid the conflict of resistors we must use non pull-up'ed D70 and D71 as SDA and SCL respectively,
// and these correspond to Wire1 lib here.
#define WIRE              Wire1
#else
// standard Wire library enables ATmega internal pull-ups by default. usually the resistors are 20k ohm or greater thus
// these should cause no harm. 
#define WIRE              Wire
#endif

// GoPro Dual Hero EEPROM IDs
const int ID_MASTER = 4;
const int ID_SLAVE  = 5;

// I2C slave addresses
const int I2CEEPROM = 0x50;
const int SMARTY = 0x60;
const int I2CPROXY = 0x70;

// Camera accesses I2C EEPROM located at slave address 0x50 using 8-bit word address.
// So any one of 3.3V EEPROMs 24XX00, 24XX01, 24XX02, 24XX04, 24XX08, or 24XX16 (XX = AA or LC) works with camera.
//   Note: If not pin-compatible 24AA00 (SOT-23) is used, PCB modifications as well as following value changes are nesessary:
//     const int WRITECYCLETIME = 4000;
//     const int PAGESIZE = 1;
//
// cycle time in milliseconds after block write
const int WRITECYCLETIME = 5000;
//
// page size for block write
const int PAGESIZE = 8; // 24XX01, 24XX02
// const int PAGESIZE = 16; // 24XX04, 24XX08, 24XX16

byte buf[MEWPRO_BUFFER_LENGTH], recv[MEWPRO_BUFFER_LENGTH];

int bufp = 1;
volatile boolean recvq = false;
unsigned long previous_sync;  // last sync (used by timelapse mode)

#if !defined(__AVR_ATtiny1634__)
// --------------------------------------------------------------------------------
#if !defined(USE_I2C_PROXY)
#if defined(__MK20DX256__) || defined(__MK20DX128__) || defined(__MKL26Z64__)
// interrupts
void receiveHandler(size_t numBytes)
#else
void receiveHandler(int numBytes)
#endif
{
  int i = 0;
  while (WIRE.available()) {
    recv[i++] = WIRE.read();
    recvq = true;
  }
  if ((recv[1] << 8) + recv[2] == SET_BACPAC_3D_SYNC_READY) {
    switch (recv[3]) {
    case 1:
      ledOn();
      break;
    case 0:
      ledOff();
      break;
    default:
      break;
    }
  }
}

void requestHandler()
{
  digitalWrite(I2CINT, HIGH);
  WIRE.write(buf, (int) buf[0] + 1);
}

void resetI2C()
{
  WIRE.begin(SMARTY);
  WIRE.onReceive(receiveHandler);
  WIRE.onRequest(requestHandler);

  emptyQueue();
}

#else // if defined(USE_I2C_PROXY)

void receiveHandler()
{
  int datalen;
  // since data length is variable and not yet known, read one byte first.
  WIRE.requestFrom(I2CPROXY, 1, I2C_NOSTOP);
  if (WIRE.available()) {
    datalen = WIRE.read() & 0x7f;
  } else {
    // panic! error
    datalen = -1;
  }
  // request again
  WIRE.requestFrom(I2CPROXY, datalen + 1, I2C_STOP);
  for (int i = 0; i <= datalen; i++) {
    recv[i] = WIRE.read();
    recvq = true;
  }
  if ((recv[1] << 8) + recv[2] == SET_BACPAC_3D_SYNC_READY) {
    switch (recv[3]) {
    case 1:
      ledOn();
      break;
    case 0:
      ledOff();
      break;
    default:
      break;
    }
  }
}

void resetI2C()
{
  WIRE.begin();
  emptyQueue();
}

#endif

// Read I2C EEPROM
boolean isMaster()
{
  if (eepromId == 0) {
    WIRE.begin();
    WIRE.beginTransmission(I2CEEPROM);
    WIRE.write((byte) 0);
    WIRE.endTransmission(I2C_NOSTOP);
    WIRE.requestFrom(I2CEEPROM, 1, I2C_STOP);
    if (WIRE.available()) {
      eepromId = WIRE.read();
    }

    resetI2C();
  }
  return (eepromId == ID_MASTER);
}

// Write I2C EEPROM
void roleChange()
{
  byte id, d;
  // emulate detouching bacpac by releasing BPRDY line
  pinMode(BPRDY, INPUT);
  delay(1000);

  id = isMaster() ? ID_SLAVE : ID_MASTER;
  
  WIRE.begin();
  for (unsigned int a = 0; a < 16; a += PAGESIZE) {
    WIRE.beginTransmission(I2CEEPROM);
    WIRE.write((byte) a);
    for (int i = 0; i < PAGESIZE; i++) {
      switch ((a + i) % 4) {
        case 0: d = id; break; // major (MOD1): 4 for master, 5 for slave
        case 1: d = 5; break;  // minor (MOD2) need to be greater than 4
        case 2: d = 1; break;
        case 3: d = (id == 4 ? 0x0a : 0x0b); break;
      }
      WIRE.write(d);
    }
    WIRE.endTransmission(I2C_STOP);
    delayMicroseconds(WRITECYCLETIME);
  }
  pinMode(BPRDY, OUTPUT);
  eepromId = id;
  digitalWrite(BPRDY, LOW);
  resetI2C();
}

// --------------------------------------------------------------------------------
#else // __AVR_ATtiny1634__

#ifdef USE_I2C_PROXY
#error do not define USE_I2C_PROXY for the CPU cannot work as I2C master
#endif

#define ROMSIZE 16
#define EEPROMOFFSET 0

volatile uint8_t wordAddr;
volatile boolean repeatedStart;
volatile boolean emulateRom;

// interrupts
boolean addressHandler(uint16_t slave, uint8_t count)
{
  emulateRom = ((slave >> 1) == I2CEEPROM);
  repeatedStart = (count > 0 ? true : false);
  if (emulateRom) {
    // EEPROM access
    if (repeatedStart && WIRE.available()) {
      wordAddr = WIRE.read();
    }
  }
  return true;
}

void receiveHandler(size_t numBytes)
{ 
  if (emulateRom) {
    // EEPROM access (byte write: ignored)
    return;
  }
  
  // SMARTY
  int i = 0;
  while (WIRE.available()) {
    recv[i++] = WIRE.read();
    recvq = true;
  }
  if ((recv[1] << 8) + recv[2] == SET_BACPAC_3D_SYNC_READY) {
    switch (recv[3]) {
    case 1:
      ledOn();
      break;
    case 0:
      ledOff();
      break;
    default:
      break;
    }
  }
}

void requestHandler()
{
  if (emulateRom) {
    // EEPROM access
    if (repeatedStart) {
      // GoPro requests random read only
      WIRE.write(EEPROM.read(wordAddr % ROMSIZE + EEPROMOFFSET));
    }
    return; 
  }
  
  // SMARTY
  digitalWrite(I2CINT, HIGH);
  WIRE.write(buf, (int) buf[0] + 1);
}

void resetI2C()
{
  WIRE.begin(I2CEEPROM, ((SMARTY << 1) | 1));
  WIRE.onAddrReceive(addressHandler);
  WIRE.onReceive(receiveHandler);
  WIRE.onRequest(requestHandler);

  emptyQueue();
}

// Read I2C EEPROM
void __romWrite(uint8_t id)
{
  byte d;
  for (int a = 0; a < ROMSIZE; a++) {
    switch (a % 4) {
      case 0: d = id; break; // major (MOD1): 4 for master, 5 for slave
      case 1: d = 5; break;  // minor (MOD2) need to be greater than 4
      case 2: d = 1; break;
      case 3: d = (id == 4 ? 0x0a : 0x0b); break;
    }
    EEPROM.write(a + EEPROMOFFSET, d);
  }
}

boolean isMaster()
{
  if (eepromId == 0) {
    eepromId = EEPROM.read(EEPROMOFFSET);
    if (eepromId != ID_MASTER && eepromId != ID_SLAVE) {
      __romWrite(ID_MASTER);
      eepromId = ID_MASTER;
    }
    resetI2C();
  }
  return (eepromId == ID_MASTER);
}

// Write built-in EEPROM
void roleChange()
{
  byte id, d;
  // emulate detouching bacpac by releasing BPRDY line
  pinMode(BPRDY, INPUT);
  delay(1000);

  id = isMaster() ? ID_SLAVE : ID_MASTER;
  __romWrite(id);
  pinMode(BPRDY, OUTPUT);
  eepromId = id;
  digitalWrite(BPRDY, LOW);
}
// --------------------------------------------------------------------------------
#endif // __AVR_ATtiny1634

// print out debug information to Arduino serial console
void __debug(const __FlashStringHelper *p)
{
  if (debug) {
    Serial.println(p);
  }
}

void printHex(uint8_t d, boolean upper)
{
  char t;
  char a = upper ? 'A' : 'a';
  t = d >> 4 | '0';
  if (t > '9') {
    t += a - '9' - 1;
  }
  Serial.print(t);
  t = d & 0xF | '0';
  if (t > '9') {
    t += a - '9' - 1;
  }
  Serial.print(t);
}

void __printBuf(byte *p)
{
  int buflen = p[0] & 0x7f;

  for (int i = 0; i <= buflen; i++) {
    if (i == 1 && isprint(p[1]) || i == 2 && p[1] != 0 && isprint(p[2])) {
      if (i == 1) {
        Serial.print(' ');
      }
      Serial.print((char) p[i]);
    } else {
      Serial.print(' ');
      printHex(p[i], false);
    }
  }
  Serial.println("");
  Serial.flush();
}

void _printInput()
{
  if (debug) {
    Serial.print('>');
    __printBuf(recv);
  }
}

void SendBufToCamera() {
  // some command need to be executed in MewPro side before sending it to camera
  int command = (buf[1] << 8) + buf[2];
  switch (command) {
  case SET_CAMERA_3D_SYNCHRONIZE:
    noInterrupts();
    waiting = true; // don't read command from the queue until a reply is received.
    previous_sync = millis();
    interrupts();
    break;
  case GET_CAMERA_INFO:
  case GET_CAMERA_SETTING:
  case SET_CAMERA_SETTING:
  case SET_CAMERA_VIDEO_OUTPUT:
  case SET_CAMERA_AUDIOINPUTMODE:
  case SET_CAMERA_USBMODE:
  case SET_CAMERA_DATE_TIME:
    waiting = true; // don't read command from the queue until a reply is received.
    break;
  default:
    for (int offset = 0x09; offset < TD_BUFFER_SIZE; offset++) {
      if (pgm_read_word(tdtable + offset - 0x09) == command) {
        td[offset] = buf[3];
        waiting = true;
        break;
      }
    }
    break;
  }
  if (debug) {
    Serial.print('<');
    __printBuf(buf);
  }
#if !defined(USE_I2C_PROXY)
  digitalWrite(I2CINT, LOW);
#else
  WIRE.beginTransmission(I2CPROXY);
  WIRE.write(buf, (int) buf[0] + 1);
  WIRE.endTransmission(I2C_STOP);
#endif
}

// SET_CAMERA_3D_SYNCHRONIZE START_RECORD
void startRecording()
{
  queueIn(F("SY1"));
}

// SET_CAMERA_3D_SYNCHRONIZE STOP_RECORD
void stopRecording()
{
  queueIn(F("SY0"));
}

// Camera power On
void powerOn()
{
  pinMode(PWRBTN, OUTPUT);
  digitalWrite(PWRBTN, LOW);
  delay(1000);
  tdDone = false;
  pinMode(PWRBTN, INPUT);
}

//MAS - Reset function which enables watchdog
void resetArduinoBoard()
{
  g_resetArduino = true;
}

void checkCameraCommands()
{
  while (inputAvailable())  {
    static boolean shiftable;
    byte c = myRead();
    switch (c) {
      case ' ':
        continue;
      case '\r':
      case '\n':
        if (bufp != 1) {
          buf[0] = bufp - 1;
          bufp = 1;
          SendBufToCamera();
        }
        return;
      case '&':
        bufp = 1;
        debug = !debug;
        __debug(F("debug messages on"));
        while (inputAvailable()) {
          if (myRead() == '\n') {
            return;
          }
        }
        return;  
      case '@':
        bufp = 1;
        __debug(F("camera power on"));
        powerOn();
        while (inputAvailable()) {
          if (myRead() == '\n') {
            return;
          }
        }
        return;
      case '$': //MAS- THIS IS WHAT I'M GONNA USE TO RESET THIS MOFO
        //Reset here
        __debug(F("mas reset triggered"));
        while (inputAvailable()) {
          if (myRead() == '\n') {
            break;
          }
        }
        resetArduinoBoard();
        return; //unreachable
      case '!':
        bufp = 1;
        __debug(F("role change"));
        roleChange();
        while (inputAvailable()) {
          if (myRead() == '\n') {
            return;
          }
        }
        return;
      case '/':
        Serial.println(F(MEWPRO_FIRMWARE_VERSION));
        Serial.flush();
        return;
      default:
        if (bufp >= 3 && isxdigit(c)) {
          c -= '0';
          if (c >= 10) {
            c = (c & 0x0f) + 9;
          }    
        }
        if (bufp < 4) {
          shiftable = true;
          buf[bufp++] = c;
        } else {
          if (shiftable) { // TM requires six args; "TM0e080a0b2d03" sets time to 2014 Aug 10 11:45:03
            buf[bufp-1] = (buf[bufp-1] << 4) + c;
          } else {
            buf[bufp++] = c;
          }
          shiftable = !shiftable;      
        }
        break;
    }
  }
}


