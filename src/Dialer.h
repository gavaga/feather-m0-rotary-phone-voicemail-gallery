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

    static void _static_handle_interrupt();
    void _handle_interrupt();

    uint32_t _pin;
    volatile bool _is_up = false;
    volatile uint8_t _ticks = 0;
    volatile uint64_t _last_tick_time = 0, _last_fall_time = 0;
};

extern DialerClass &Dialer; 

#endif // DIALER_H_