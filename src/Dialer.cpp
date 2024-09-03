#include <Arduino.h>

#include "io.h"
#include "Dialer.h"

#define PULSE_TIMEOUT 350000
#define PULSE_WIDTH_MIN 30000
#define PULSE_WIDTH_MAX 90000

DialerClass &DialerClass::getInstance()
{
    static DialerClass instance;
    return instance;
}

DialerClass::DialerClass() {}

bool DialerClass::init(uint32_t pin)
{
    _pin = pin;
    pinMode(_pin, INPUT);

    cout << F("Dialer initialized on pin ") << pin << endl;

    return true;
}

bool DialerClass::check_dialed(uint32_t *number)
{
    _check_change();

    if (_last_tick_time > 0 && _ticks > 0 && micros() - _last_tick_time > PULSE_TIMEOUT) {
        *number = _ticks;
        _ticks = 0;
        return true;
    }

    return false;
}

void DialerClass::_check_change()
{
    uint32_t val = digitalRead(_pin);
    if (val != _last_val) {
        // falling
        if (_is_up && val == LOW)
        {
            _last_fall_time = micros();
        }

        // set the latest rise time
        else if (!_is_up && val == HIGH)
        {
            uint64_t time = micros();
            uint64_t pulse_width = time - _last_fall_time;
            if (pulse_width > PULSE_WIDTH_MIN && pulse_width < PULSE_WIDTH_MAX)
            {
                _last_tick_time = time;
                _ticks++;
            }
        }

        _is_up = val == HIGH;
    }
    _last_val = val;
}

DialerClass &Dialer = DialerClass::getInstance();