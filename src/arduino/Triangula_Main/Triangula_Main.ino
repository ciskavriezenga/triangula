
// Comment this out to entirely disable the motor control functions. Handy when testing and you
// really don't want the robot to vanish off into the distance mid-test.
#define ENABLE_MOTOR_FUNCTIONS

/*
   Simple sketch to directly control the Syren drivers. Send an I2C message containing 3 bytes, one
   for each motor, these are converted to values in the range -128 to 127 and sent to the motor drivers.
   Also handles lighting and reporting of encoder values in response to a bulk data request.
*/
#include <Adafruit_NeoPixel.h>
#include "Triangula_NeoPixel.h"
#include <Wire.h>
#ifdef ENABLE_MOTOR_FUNCTIONS
#include <Sabertooth.h>
#endif

// Address to receive messages from the I2C master
#define SLAVE_ADDRESS  0x70

// Command codes
#define MOTOR_SPEED_SET 0x20
#define SET_SOLID_COLOUR 0x21
#define ENCODER_READ 0x22

// Register map array size in bytes
#define REG_MAP_SIZE   6
// Maximum length of a command
#define MAX_SENT_BYTES 5

// Track absolute encoder values, these are unsigned ints and can (and will) roll over. The difference()
// function handles these cases properly. Note that these are volatile as they're updated on pin change
// interrupts, when reading them you should disable interrupts or risk reading half-way through an update.
volatile unsigned int pos_a = 0;
volatile unsigned int pos_b = 0;
volatile unsigned int pos_c = 0;

byte registerMap[REG_MAP_SIZE];
byte receivedCommands[MAX_SENT_BYTES];
volatile boolean newDataAvailable = false;

#ifdef ENABLE_MOTOR_FUNCTIONS
// Motor drivers, must be configured in packet serial mode with addresses 128, 129 and 130
Sabertooth ST[3] = { Sabertooth(130), Sabertooth(129), Sabertooth(128) };
#endif

// Lights
Triangula_NeoPixel pixels = Triangula_NeoPixel();

void setup() {
  pixels.begin();
  Serial.begin(9600);
  Wire.begin(SLAVE_ADDRESS);
  Wire.onRequest(requestEvent);
  Wire.onReceive(receiveEvent);
  // Start up connection to Syren motor controllers, setting baud rate
  // then waiting two seconds to allow the drivers to power up and sending
  // the auto-calibration signal to all controllers on the same bus
#ifdef ENABLE_MOTOR_FUNCTIONS
  SabertoothTXPinSerial.begin(9600);
  Sabertooth::autobaud(SabertoothTXPinSerial);
#endif
  // Stop interrupts
  cli();
  // Configure port 0, pins 8-13, as inputs
  // Note - this requires a 10k pullup between 5V and pin 13
  for (int pin = 8; pin <= 13; pin++) {
    pinMode(pin, INPUT);
    digitalWrite(pin, HIGH);
  }
  // Enable interrupts on port 0
  PCICR |= (1 << PCIE0);
  // Un-mask all pins
  PCMSK0 = 63;
  // Enable interrupts
  sei();

#ifdef ENABLE_MOTOR_FUNCTIONS
  for (int i = 0; i < 3; i++) {
    ST[i].motor(0);
  }
#endif
  pixels.setSolidColour(170, 255, 60);
  pixels.show();
}


volatile int encoderData[6];
volatile int encoderIndex = 6;

void loop() {
  if (newDataAvailable) {
    newDataAvailable = false;
    uint8_t i2c_command = receivedCommands[0];
    switch (i2c_command) {
      case MOTOR_SPEED_SET:
#ifdef ENABLE_MOTOR_FUNCTIONS
        if (checkCommand(3)) {
          for (int i = 1; i < MAX_SENT_BYTES; i++) {
            registerMap[i - 1] = receivedCommands[i];
          }
          for (int i = 0; i < 3; i++) {
            ST[i].motor(((int)(receivedCommands[i + 1])) - 128);
          }
          setColours(registerMap, REG_MAP_SIZE, 8, 0);
        }
#endif
        break;
      case SET_SOLID_COLOUR:
        if (checkCommand(3)) {
          pixels.setSolidColour(receivedCommands[1], receivedCommands[2], receivedCommands[3]);
          pixels.show();
        }
        break;
      case ENCODER_READ:
        encoderData[0] =  (pos_c & 0xff00) >> 8;
        encoderData[1] =  pos_c & 0xff;
        encoderData[2] =  (pos_b & 0xff00) >> 8;
        encoderData[3] =  pos_b & 0xff;
        encoderData[4] =  (pos_a & 0xff00) >> 8;
        encoderData[5] =  pos_a & 0xff;
        encoderIndex = 0;
        break;
      default:
#ifdef ENABLE_MOTOR_FUNCTIONS
        // Unknown command, stop the motors.
        for (int i = 0; i < 3; i++) {
          ST[i].motor(0);
        }
#endif
        pixels.setSolidColour(0, 255, 50);
        pixels.show();
        break;
    }
  }
}


// Called on I2C data request
void requestEvent() {
  if (encoderIndex < 6) {
    Wire.write(encoderData[encoderIndex++]);
  }
  else {
    Wire.write(0);
  }
}

// Validate a command with x bytes plus a register, implying a checksum byte at recievedCommands[x+1]
boolean checkCommand(uint8_t command_length) {
  uint8_t checksum = 0;
  for (int a = 0; a <= command_length; a++) {
    checksum ^= receivedCommands[a];
  }
  return checksum == receivedCommands[command_length + 1];
}

// Called on I2C data reception
void receiveEvent(int bytesReceived) {
  for (int a = 0; a < bytesReceived; a++) {
    if (a < MAX_SENT_BYTES) {
      receivedCommands[a] = Wire.read();
    }
    else {
      Wire.read();  // if we receive more data then allowed just throw it away
    }
  }
  newDataAvailable = true;
}

// Calculate the difference between two encoder readings, allowing for values which wrap. Will be positive
// if the second reading is larger than the first.
int difference(unsigned int a, unsigned int b) {
  int d = b - a;
  if (abs(d) > 0x7FFFu) {
    d = 0xFFFFu - (b + a);
  }
  return d;
}

/*
  Sequence of pulses clockwise is 0,1,3,2
  Assuming that the first state is the two LSB of a four bit value
  So we have the following returning '1'
  0->1 : 4
  1->3 : 13
  3->2 : 11
  2->0 : 2
  In the opposite direction we have 2,3,1,0
  2->3 : 14
  3->1 : 7
  1->0 : 1
  0->2 : 8
  All other values should return 0 as they don't represent valid state transitions
*/
const int encoderValues[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

// Track previous encoder states
byte encoder_a;
byte encoder_b;
byte encoder_c;

// Handle pin change interrupts
ISR (PCINT0_vect) {
  byte last_read = PINB;
  byte a = (last_read & 48) >> 4;
  byte b = (last_read & 12) >> 2;
  // The wires are swapped over on encoder c (the green pylon) so we need to swap things!
  byte c = last_read & 3;
  pos_a += encoderValues[a + (encoder_a << 2)];
  pos_b += encoderValues[b + (encoder_b << 2)];
  pos_c -= encoderValues[c + (encoder_c << 2)];
  encoder_a = a;
  encoder_b = b;
  encoder_c = c;
}

// Set colours based on the signal received from the Pi
void setColours(byte hues[], int hueCount, int pixelsPerValue, int fromPixel) {
  int pixel = fromPixel;
  for (int i = 0; i < hueCount; i++) {
    uint32_t colour_a = pixels.hsvToColour(hues[i], 255, 150);
    uint32_t colour_b = pixels.hsvToColour(hues[(i + 1) % hueCount], 255, 150);
    for (int j = 0; j < pixelsPerValue; j++) {
      uint32_t colour = pixels.interpolate(colour_a, colour_b, ((float)j) / ((float)pixelsPerValue));
      pixels.setPixelColor(pixel++, colour);
    }
  }
  pixels.show();
}

