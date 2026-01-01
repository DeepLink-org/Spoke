#include "../csrc/spoke/actor.h"
#include <cmath>

const spoke::Action kAddTen = (spoke::Action)0x11;
const spoke::Action kSquare = (spoke::Action)0x12;

class WorkerActor: public spoke::Actor {
public:
    WorkerActor(const std::string& id, int rx, int tx): spoke::Actor(id, rx, tx) {}

    SPOKE_METHOD(WorkerActor, addTen, kAddTen)
    {
        return val + 10.0;
    }

    SPOKE_METHOD(WorkerActor, square, kSquare)
    {
        return val * val;
    }
};

SPOKE_REGISTER_ACTOR("WorkerActor", WorkerActor);

