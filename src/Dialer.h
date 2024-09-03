#ifndef DIALER_H_
#define DIALER_H_

#include <stdint.h>

class DialerClass {
public:
    static DialerClass& getInstance();
    DialerClass(DialerClass&) = delete;

    bool init(uint32_t pin);
    bool check_dialed(uint32_t* number);

private:
    DialerClass();

    void _check_change();

    uint32_t _pin;
    uint32_t _last_val = HIGH;
    bool _is_up = true;
    uint8_t _ticks = 0;
    uint64_t _last_tick_time = 0, _last_fall_time = 0;
};

extern DialerClass &Dialer; 

#endif // DIALER_H_