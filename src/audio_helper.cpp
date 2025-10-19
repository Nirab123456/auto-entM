#include "audio_helper.h"




void IRAM_ATTR button_isr_handler()
{
    TickType_t now = xTaskGetTickCountFromISR();
    //debounce
    if((now -isr_last_edge_tick)<debounce_ticks)
    {
        isr_last_edge_tick =now;
        return;
    }
    isr_last_edge_tick = now;

    int state = digitalRead(RESET_WIFI_BUTTON_PIN);
    if (state == LOW)
    {
        isr_press_tick = now ;
        return;
    }
    else
    {
        TickType_t press_tick = isr_press_tick;
        if (press_tick==0)
        {
            return;
        }
        TickType_t dur_ticks = now - press_tick;
        uint32_t dur_ms = (uint32_t)pdTICKS_TO_MS(dur_ticks);

        if (dur_ms >= BUTTON_HOLD_MS)
        {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            if (button_semaphore)
            {
                xSemaphoreGiveFromISR(button_semaphore,&xHigherPriorityTaskWoken);
                if (xHigherPriorityTaskWoken==pdTRUE)
                {
                    portYIELD_FROM_ISR();
                }   
            }
        }
        isr_press_tick = 0;
    }
}


void button_handleTASK(void*pv)
{
    (void)pv;
    for(;;)
    {
        if(button_semaphore==NULL)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (xSemaphoreTake(button_semaphore,portMAX_DELAY))
        {
            Serial.println("BUTTON: LOng press detected --> starting configaration portal");
            startconfigportal_button();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    vTaskDelete(NULL);
}

void setup_button_isr_TASK()
{
    //create
    if (button_semaphore == NULL)
    {
        button_semaphore = xSemaphoreCreateBinary();
        if (button_semaphore == NULL)
        {
            Serial.println("BUTTON: SEMAPHORE : Creation failed");
            return;
        }
    }
    
    pinMode(RESET_WIFI_BUTTON_PIN,INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(RESET_WIFI_BUTTON_PIN),button_isr_handler,CHANGE);

    BaseType_t ok = xTaskCreate(button_handleTASK,"buttob_handle_task",3072,NULL,3,NULL);
    if (ok!=pdPASS)
    {
        Serial.println("BUTTON: Task creation failed");
    }
    else
    {
        Serial.println("BUTTON: Task created");
    }
}


void monitorTASK(void*pv)
{
    (void)pv;
    for(;;)
    {
        vTaskDelay(DELAYTICKS);
        if (audiohandleTASK == NULL || networkhandleTASK==NULL)
        {
            Serial.println("TASK MONITOR : Task missing");
            startTASK();
        }
        if (audiohandleTASK != NULL)
        {
            UBaseType_t hw = uxTaskGetStackHighWaterMark(audiohandleTASK);
            Serial.printf("MONITOR TASK : AUDIO TASK :High watermark %u\n",(unsigned)hw);
        }
    }
    vTaskDelete(NULL);
}

void startmonitorTASK()
{
    if(monitorhandleTASK==NULL)
    {
        BaseType_t ok = xTaskCreatePinnedToCore(
            monitorTASK,
            "monitor_task",
            MONITOR_STACK,
            NULL,
            2,
            &monitorhandleTASK,
            0
        );
        if(ok!=pdPASS)
        {
            Serial.println("MONITOR TASK :Creation failed");
            monitorhandleTASK = NULL;
        }
        else
        {
            Serial.println("MONITOR TASK : Created");
        }
    }
    else{
        Serial.println("MONITOR TASK : Already running");
    }
}

void printhandleTASK()
{
    if (printtaskHANDLE==NULL)
    {
        BaseType_t ok = xTaskCreatePinnedToCore(
            printTASK,"print_task",
            PRINT_STACK,NULL,1,
            &printtaskHANDLE,1
        );
        if (ok == pdPASS)
        {
            Serial.println("PRINT TASK : Created");
        }
        else
        {
            Serial.println("PRINT TASK : Creation failed");
            printtaskHANDLE = NULL;
        }
    }
    else
    {
        Serial.println("PRINT TASK : Already running");
    }
    
}