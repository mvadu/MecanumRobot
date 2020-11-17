#include <Arduino.h>
#include <AccelStepper.h>
#include <commondata.h>
#include <Wire.h>

#define EN 8

//Direction pin
#define FR_DIR 13
#define FL_DIR 6
#define RR_DIR 7
#define RL_DIR 5

//Step pin
#define FR_STP 12
#define FL_STP A4
#define RR_STP 4
#define RL_STP A5

//DRV8825
int delayTime = 15; //Delay between each pause (uS)
int stps = 2001;    // Steps to move

AccelStepper FrontRightWheel(1, FR_STP, FR_DIR); // (Type:driver, STEP, DIR)
AccelStepper FrontLeftWheel(1, FL_STP, FL_DIR);  // (Type:driver, STEP, DIR)
AccelStepper BackRightWheel(1, RR_STP, RR_DIR);  // (Type:driver, STEP, DIR)
AccelStepper BackLeftWheel(1, RL_STP, RL_DIR);   // (Type:driver, STEP, DIR)

DirMessage msg(Direction::Stop);

Direction currentDir;
int wheelSpeed = 3000;
volatile bool newDataReceived = false;
unsigned long stoppedTs;
bool disabled = true, stopped = true;
Command lastCommand = Command::NotSet;

void moveForward();
void moveBackward();
void moveSidewaysRight();
void moveSidewaysLeft();
void rotateLeft();
void rotateRight();
void moveRightForward();
void moveRightBackward();
void moveLeftForward();
void moveLeftBackward();
void stopMoving();
void requestEvent();
void receiveEvent(int numBytes);
void enableMotors();
void disableMotors();

void setup()
{
  Serial.begin(9600);
  while (!Serial && millis() < 15000)
  {
    delay(100);
    ; // wait for serial port to connect. Needed for native USB port only
  }
  // Serial.println("Ready to listen");

  Wire.begin(motorController);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);
  //turn off internal 5V pull ups. ESP32 will pull it to 3V.
  digitalWrite(SDA, LOW);
  digitalWrite(SCL, LOW);

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(EN, OUTPUT);
  enableMotors();
  delay(2000);
  disableMotors();

  FrontRightWheel.setMaxSpeed(4000);
  FrontLeftWheel.setMaxSpeed(4000);
  BackRightWheel.setMaxSpeed(4000);
  BackLeftWheel.setMaxSpeed(4000);
}

void loop()
{

  if (newDataReceived)
  {
    Serial.print(millis());
    Serial.print(" New direction receieved ");
    Serial.println((uint8_t)msg.dir);

    currentDir = msg.dir;
    if (disabled && msg.dir != Direction::Stop)
    {
      Serial.print(millis());
      Serial.println(" enable motors");
      enableMotors();
    }
    newDataReceived = false;
    stopped = false;
  }
  if (!disabled)
  {
    switch (currentDir)
    {
    case Direction::Forward:
      moveForward();
      break;
    case Direction::Backward:
      moveBackward();
      break;
    case Direction::Right:
      moveSidewaysRight();
      break;
    case Direction::Left:
      moveSidewaysLeft();
      break;
    case Direction::RotateLeft:
      rotateLeft();
      break;
    case Direction::RotateRight:
      rotateRight();
      break;
    case Direction::DiagonalRightForward:
      moveRightForward();
      break;
    case Direction::DiagonalRightBackward:
      moveRightBackward();
      break;
    case Direction::DiagonalLeftForward:
      moveLeftForward();
      break;
    case Direction::DiagonalLeftBackward:
      moveLeftBackward();
      break;
    default:
      stopMoving();
      break;
    }

    // Execute the steps
    FrontLeftWheel.runSpeed();
    BackLeftWheel.runSpeed();
    FrontRightWheel.runSpeed();
    BackRightWheel.runSpeed();

    if (stopped && millis() - stoppedTs > 3000)
    {
      disableMotors();
      Serial.print(millis());
      Serial.println(" disable motors");
    }
  }
}

void requestEvent()
{
  switch (lastCommand)
  {
  case Command::PresenceCheck:
    Wire.write(motorController);
    break;
  case Command::ReadBattery:
  {
    int sensorValue = analogRead(A0);
    float voltage = sensorValue * (5.0 / 1023.00) * 3; // Convert the reading values from 5v to suitable 12V
    Wire.write((const uint8_t *)&voltage, sizeof(voltage));
  }
  break;
  case Command::DirectionCheck:
    Wire.write((uint8_t)msg.dir);
    break;
  default:
    break;
  }
}

void receiveEvent(int numBytes)
{
  // Serial.print("-");
  // Serial.print(numBytes);
  Wire.readBytes((byte *)&lastCommand, 1);
  switch (lastCommand)
  {
  case Command::SetDirection:
    Wire.readBytes((byte *)&msg.dir, sizeof(msg.dir));
    newDataReceived = true;
    break;
  default:
    break;
  }
}

void moveForward()
{
  FrontLeftWheel.setSpeed(wheelSpeed);
  BackLeftWheel.setSpeed(wheelSpeed);
  FrontRightWheel.setSpeed(-wheelSpeed);
  BackRightWheel.setSpeed(-wheelSpeed);
}

void moveBackward()
{
  FrontLeftWheel.setSpeed(-wheelSpeed);
  BackLeftWheel.setSpeed(-wheelSpeed);
  FrontRightWheel.setSpeed(wheelSpeed);
  BackRightWheel.setSpeed(wheelSpeed);
}

void moveSidewaysRight()
{
  FrontRightWheel.setSpeed(-wheelSpeed);
  BackRightWheel.setSpeed(wheelSpeed);
  FrontLeftWheel.setSpeed(-wheelSpeed);
  BackLeftWheel.setSpeed(wheelSpeed);
}

void moveSidewaysLeft()
{
  FrontRightWheel.setSpeed(wheelSpeed);
  BackRightWheel.setSpeed(-wheelSpeed);
  FrontLeftWheel.setSpeed(wheelSpeed);
  BackLeftWheel.setSpeed(-wheelSpeed);
}

void rotateLeft()
{
  FrontLeftWheel.setSpeed(-wheelSpeed);
  BackLeftWheel.setSpeed(-wheelSpeed);
  FrontRightWheel.setSpeed(-wheelSpeed);
  BackRightWheel.setSpeed(-wheelSpeed);
}

void rotateRight()
{
  FrontLeftWheel.setSpeed(wheelSpeed);
  BackLeftWheel.setSpeed(wheelSpeed);
  FrontRightWheel.setSpeed(wheelSpeed);
  BackRightWheel.setSpeed(wheelSpeed);
}

void moveRightForward()
{
  FrontLeftWheel.setSpeed(wheelSpeed);
  BackLeftWheel.setSpeed(0);
  FrontRightWheel.setSpeed(0);
  BackRightWheel.setSpeed(-wheelSpeed);
}

void moveRightBackward()
{
  FrontLeftWheel.setSpeed(0);
  BackLeftWheel.setSpeed(-wheelSpeed);
  FrontRightWheel.setSpeed(wheelSpeed);
  BackRightWheel.setSpeed(0);
}

void moveLeftForward()
{
  FrontLeftWheel.setSpeed(0);
  BackLeftWheel.setSpeed(wheelSpeed);
  FrontRightWheel.setSpeed(-wheelSpeed);
  BackRightWheel.setSpeed(0);
}

void moveLeftBackward()
{
  FrontLeftWheel.setSpeed(-wheelSpeed);
  BackLeftWheel.setSpeed(0);
  FrontRightWheel.setSpeed(0);
  BackRightWheel.setSpeed(wheelSpeed);
}

void stopMoving()
{

  FrontLeftWheel.setSpeed(0);
  BackLeftWheel.setSpeed(0);
  FrontRightWheel.setSpeed(0);
  BackRightWheel.setSpeed(0);
  if (!stopped)
  {
    stoppedTs = millis();
    stopped = true;
    Serial.print(stoppedTs);
    Serial.println(" Stopped");
  }
}

void enableMotors()
{
  digitalWrite(EN, LOW);
  digitalWrite(LED_BUILTIN, HIGH);
  disabled = false;
}

void disableMotors()
{
  digitalWrite(EN, HIGH);
  digitalWrite(LED_BUILTIN, LOW);
  disabled = true;
}