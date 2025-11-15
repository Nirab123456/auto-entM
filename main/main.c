#include "Arduino.h" // or declare initArduino prototype
extern void arduino_setup_call(void);

void app_main(void)
{
    initArduino();
    arduino_setup_call();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
