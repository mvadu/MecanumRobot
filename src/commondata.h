#ifndef _COMMON_h
#define _COMMON_h

enum class Direction : uint8_t
{
    NotSet = 0,
    DiagonalLeftForward = 1,
    Forward = 2,
    DiagonalRightForward = 3,
    Left = 4,
    Stop = 5,
    Right = 6,
    DiagonalLeftBackward = 7,
    Backward = 8,
    DiagonalRightBackward = 9,
    RotateLeft = 10,
    RotateRight = 11
};
static_assert(sizeof(Direction) == 1, "!");

enum class Command : uint8_t //enum __attribute__((__packed__))
{
    NotSet = 0,
    PresenceCheck = 0xAA,
    ReadBattery = 0x10,
    DirectionCheck = 0xB0,
    SetDirection = 0xB1,
    SpeedCheck = 0xC0,
    SetSpeed = 0xC1,
    
};
static_assert(sizeof(Command) == 1, "!");

const uint8_t motorController = 0xA;

class DirMessage
{
public:
    const Command cmd = Command::SetDirection;
    Direction dir;
    DirMessage(Direction d) : dir(d)
    {
    }
};

class SpeedMessage
{
public:
    const Command cmd = Command::SetSpeed;
    uint16_t speed;
    uint16_t acceleration;
    SpeedMessage(uint16_t a, uint16_t s)
    {
        acceleration = a;
        speed = s;
    }
};
#endif