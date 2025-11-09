#include "headers/ReciverConfig.h"

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


void ReciverConfig::setStartConfPortalCallback(std::function<void()>cb)
{
    startConfigPortalCb_ = std::move(cb);
}
