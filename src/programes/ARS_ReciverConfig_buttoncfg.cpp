#include "headers/ReciverConfig.h"


//ISR
void IRAM_ATTR ReciverConfig::confButtonIsrHandle()
{
    BaseType_t woken = pdFALSE;
    if (button_task_handle_)
    {
        vTaskNotifyGiveFromISR(button_task_handle_, &woken);
        if (woken == pdTRUE)
        {
            portYIELD_FROM_ISR();
        }   
    }   
}

bool ReciverConfig::StartConfigPortal(const char* ap_ssid, const char* ap_password)
{
    Serial.println("ReciverConfig::startConfigPortal: preparing to start portal");
    bool was_paused = false;

    if (audio_rs_class_ptr_)
    {
        Serial.println("ReciverConfig::StartConfigPortal->PauseNetworkStreaming:Pausing audio");
        audio_rs_class_ptr_->PauseNetworkStreaming();
    }
    
    WiFiManager wm;

    bool ok;
    if (!ap_ssid || strlen(ap_ssid) == 0)
    {
        ok = wm.startConfigPortal();
    }
    else
    {
        if (!ap_password || strlen(ap_password) == 0)
        {
            wm.startConfigPortal(ap_ssid);
        }
        else
        {
            ok = wm.startConfigPortal(ap_ssid, ap_password);
        }
    }
    
    if (ok) {
        Serial.println("ReciverConfig::startConfigPortal: portal returned success (credentials acquired or already connected)");
    } else {
        Serial.println("ReciverConfig::startConfigPortal: portal failed or timed out");
    }
    
    if (audio_rs_class_ptr_)
    {
        bool reconnect_ok = audio_rs_class_ptr_ ->ReqNetworkReconnect();
        Serial.printf("ReciverConfig::AUDIO_RS -> ReqNetworkReconnect:Returned %d\n", reconnect_ok ? 1 : 0);

        if (reconnect_ok)
        {
            audio_rs_class_ptr_ ->set_consumer_ready_flag(true);
        }
        

    }
    return ok;
}

void ReciverConfig::ConfRstButtonTrampoline(void* pv)
{
    ReciverConfig* self = static_cast<ReciverConfig*>(pv);
    if (!self)
    {
        vTaskDelete(nullptr);
        return;
    }
    self->ConfButtonTaskLoop();
}

void ReciverConfig::ConfButtonTaskLoop()
{
    TickType_t press_ticks = 0;
    TickType_t last_edge_tick = 0;
    uint8_t pin = prefs_rst_open_portal_pin_;
    const TickType_t debounce_ticks = debounce_ticks_;
    uint32_t hold_ms = hold_ms_;

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (stopping_.load(std::memory_order_acquire))
        {
            break;
        }
        TickType_t now = xTaskGetTickCount();

        if ((now - last_edge_tick) < debounce_ticks)
        {
            last_edge_tick = now;
            continue;
        }
        
        last_edge_tick = now;
        int level = digitalRead(pin);
        if (level == LOW)
        {
            if (press_ticks == 0)
            {
                press_ticks = now;
            }
            continue;
        }
        else
        {
            if (press_ticks == 0)
            {
                continue;
            }
            TickType_t dur_ms = now - press_ticks;
            press_ticks = 0;

            if (dur_ms >= hold_ms)
            {
                Serial.print("ReciverConfig::ConfButtonTaskLoopp::Press Duration : ");
                Serial.println((unsigned)dur_ms);

                ClearPrefs();

                if (startConfigPortalCb_)
                {
                    startConfigPortalCb_();
                }
                else
                {
                    Serial.println("ReciverConfig: startConfigPortalCallback not set");
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            else
            {
                Serial.printf("ReciverConfig: short press (~%u ms) — ignored\n", (unsigned)dur_ms);
            }
        } 
    }
    StopAndClean();
    vTaskDelete(nullptr);

}


bool ReciverConfig::AttachResetButton(
    uint8_t pin,
    TickType_t debounce_ms,
    uint32_t hold_ms,
    UBaseType_t task_prio,
    uint32_t task_stack_bytes,
    BaseType_t core,
    void* arg
)
{
    
    if (pin == 0xff)
    {
        return false;
    }
    prefs_rst_open_portal_pin_ = pin; 
    if (button_task_handle_ == nullptr)
    {
        BaseType_t ok = pdFAIL;
        if (!audio_rs_class_ptr_)
        {
            ok = start_task_fn_ptr();//set parameters in header and use the function   
        }
        else
        {
            ok = audio_rs_class_ptr_->start_task(
                "ConfButtonTaskLoop",
                task_stack_bytes,
                task_prio,
                core,
                ConfRstButtonTrampoline,
                arg,
                &button_task_handle_  
            );
        }
        if (ok != pdPASS || button_task_handle_ == nullptr)
        {
            Serial.println("ReciverConfig::AttachResetButton -> ConfButtonTaskLoop::Failed creation");
            button_task_handle_ = nullptr;
            audio_rs_class_ptr_->conf_portal_rst_button_handler_ = nullptr;
            return false;
        }
        audio_rs_class_ptr_->conf_portal_rst_button_handler_ = button_task_handle_;
    }
    pinMode(prefs_rst_open_portal_pin_, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(prefs_rst_open_portal_pin_), ReciverConfig::confButtonIsrHandle, CHANGE);
    Serial.print("ReciverConfig::AttachResetButton:: Button interrupt on pin ->");
    Serial.print(prefs_rst_open_portal_pin_);
    Serial.print("\n");
    return true;
    
}

void ReciverConfig::ClearPrefs()
{
    prefs_.clear();
    prefs_.end();
    prefs_.begin(prefs_namespace_,false);
    Serial.println("ReciverConfig::ClearPrefs::Preferances cleared");
}

void ReciverConfig::DetachResetButton(TickType_t wait_ms)
{
    stopping_.store(true,std::memory_order_release);
    if (prefs_rst_open_portal_pin_ != 0xff)
    {
        detachInterrupt(digitalPinToInterrupt(prefs_rst_open_portal_pin_));
    }
    if (button_task_handle_)
    {
        xTaskNotifyGive(button_task_handle_);
        const TickType_t start = xTaskGetTickCount();
        const TickType_t wait_ticks = wait_ms;
        while (wait_ms != portMAX_DELAY && (xTaskGetTickCount() - start) < wait_ticks)
        {
            if (eTaskGetState(button_task_handle_) == eDeleted)
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        button_task_handle_ = nullptr;
        if (audio_rs_class_ptr_->conf_portal_rst_button_handler_)
        {
            audio_rs_class_ptr_->conf_portal_rst_button_handler_ = nullptr;
        }
    }
    stopping_.store(false,std::memory_order_release);
}

void ReciverConfig::StopAndClean()
{
    if (prefs_rst_open_portal_pin_ != 0xff)
    {
        detachInterrupt(digitalPinToInterrupt(prefs_rst_open_portal_pin_));
    }
    prefs_rst_open_portal_pin_ = 0xff;
    Serial.println("ReciverConfig::StopAndClean: buttonTaskLoop exiting and cleaned up");
    
}


void ReciverConfig::setStartConfPortalCallback(std::function<void()>cb)
{
    startConfigPortalCb_ = std::move(cb);
}

void ReciverConfig::StartConfPortalTrampoline(void* pv)
{
    ReciverConfig* self = static_cast<ReciverConfig*>(pv);
    if (!self)
    {
        vTaskDelete(nullptr);
        return;
    }
    self -> StartConfigPortal();
}
