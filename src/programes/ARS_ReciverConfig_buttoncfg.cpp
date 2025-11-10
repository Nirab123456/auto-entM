#include "headers/ReciverConfig.h"


//ISR
void IRAM_ATTR ReciverConfig::confButtonIsrHandle()
{
    BaseType_t woken = pdFALSE;
    if (ConfSRButtonTaskHandle_)
    {
        vTaskNotifyGiveFromISR(ConfSRButtonTaskHandle_, &woken);
        if (woken == pdTRUE)
        {
            portYIELD_FROM_ISR();
        }   
    }   
}

bool ReciverConfig::GSVIpPort(
    char* ip_buffer,
    char* port_buffer,
    bool force_start_conf_portal,
    const char* ap_ssid,
    const char* ap_password,
    uint8_t ip_buffer_len,
    uint8_t port_buffer_len
)
{
    //take mutex get ip and port
    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(prefs_namespace_, true);
        String saved_ip = prefs_.getString(PREFS_IP_ID,"");
        String saved_port = prefs_.getString(PREFS_PORT_ID,"");
        prefs_.end();

        if (saved_ip.length() > 0)
        {
            saved_ip.toCharArray(ip_buffer, ip_buffer_len);
        }
        if (saved_port.length() > 0)
        {
            saved_port.toCharArray(port_buffer, port_buffer_len);
        }
    }

    WiFiManager wm;

    WiFiManagerParameter ip_param(
        PREFS_IP_ID,
        PREFS_IP_LABEL,
        ip_buffer,
        ip_buffer_len
    );

    WiFiManagerParameter port_param(
        PREFS_PORT_ID,
        PREFS_PORT_LABEL,
        port_buffer,
        port_buffer_len
    );

    wm.addParameter(&ip_param);
    wm.addParameter(&port_param);

    bool ok = false;

    if (force_start_conf_portal)
    {
        if(ap_ssid && strlen(ap_ssid) > 0)
        {
            if (ap_password && strlen(ap_password) > 0)
            {
                ok = wm.startConfigPortal(ap_ssid, ap_password);
            }
            else
            {
                ok = wm.startConfigPortal(ap_ssid);
            }
        }
        else
        {
            ok = wm.startConfigPortal(/*random name no pass*/);
        }
    }
    else
    {
        ok = wm.autoConnect();
    }
    
    if (ok) {
        Serial.println("ReciverConfig::StartConfigPortal: portal exited (success or user closed)");
    } else {
        Serial.println("ReciverConfig::StartConfigPortal: portal failed or timed out");
    }

    const char* entered_ip = ip_param.getValue();
    const char* entered_port = port_param.getValue();

    if (entered_ip && entered_ip[0])
    {
        IPAddress tmp;
        if (tmp.fromString(String(entered_ip)))
        {
            unsigned p = 0;
            if (entered_port && entered_port[0])
            {
                p = (unsigned)atoi(entered_port);
            }
            if (p > 0 && p <= 655535)
            {
                save(entered_ip, static_cast<uint16_t> (p)); // new ip and port
                Serial.printf("ReciverConfig::StartConfigPortal - saved receiver IP=%s PORT=%u\n", entered_ip, (unsigned)p);
            }
            else
            {
                p = NULL;
                //best effort - have to fix ReciverConfig::save to save  even 1 keepin other same as previous
                save(entered_ip, p); // keep previous port new ip 
            }   
        }
    }
    else if (entered_port && entered_port[0])
    {
        unsigned p = 0;
        p = (unsigned)atoi(entered_port);
        if (p > 0 && p<= 655535)
        {
            entered_ip = nullptr;
            save(entered_ip, static_cast<uint16_t>(p)); // new port
        }
    }else {
        Serial.println("ReciverConfig::StartConfigPortal:: Both IP and Port missing");
        return false;
    }
    return true;
}

bool ReciverConfig::StartConfigPortal(bool force_start_conf_portal, const char* ap_ssid, const char* ap_password)
{
    Serial.println("ReciverConfig::startConfigPortal: preparing to start portal");
    bool was_paused = false;

    if (audio_rs_class_ptr_)
    {
        Serial.println("ReciverConfig::StartConfigPortal->PauseNetworkStreaming:Pausing audio");
        audio_rs_class_ptr_->PauseNetworkStreaming();
    }
    

    char ip_buffer[DEFAULT_IP_BUFFER_SIZE] = {0};
    char port_buffer[DEFAULT_PORT_BUFFER_SIZE] = {0};

    GSVIpPort(
        ip_buffer, 
        port_buffer, 
        force_start_conf_portal, 
        ap_ssid,ap_password, 
        DEFAULT_IP_BUFFER_SIZE, 
        DEFAULT_PORT_BUFFER_SIZE
    );
    

    {
        std::lock_guard<std::mutex> lock(mu_);
        prefs_.begin(prefs_namespace_,true);
        String SavedIP = prefs_.getString("pc_ip","");
        String SavedPort = prefs_.getString("pc_port","");
        prefs_.end();
        if (SavedIP.length() > 0)
        {
            SavedIP.toCharArray(ip_buffer,sizeof(ip_buffer));
        }
        
        if (SavedPort.length() > 0)
        {
            SavedPort.toCharArray(port_buffer,sizeof(port_buffer));
        }
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
        else
        {
            audio_rs_class_ptr_ ->set_consumer_ready_flag(false);
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
    if (ConfSRButtonTaskHandle_ == nullptr)
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
                &ConfSRButtonTaskHandle_  
            );
        }
        if (ok != pdPASS || ConfSRButtonTaskHandle_ == nullptr)
        {
            Serial.println("ReciverConfig::AttachResetButton -> ConfButtonTaskLoop::Failed creation");
            ConfSRButtonTaskHandle_ = nullptr;
            audio_rs_class_ptr_->conf_portal_rst_button_handler_ = nullptr;
            return false;
        }
        audio_rs_class_ptr_->conf_portal_rst_button_handler_ = ConfSRButtonTaskHandle_;
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
    if (ConfSRButtonTaskHandle_)
    {
        xTaskNotifyGive(ConfSRButtonTaskHandle_);
        const TickType_t start = xTaskGetTickCount();
        const TickType_t wait_ticks = wait_ms;
        while (wait_ms != portMAX_DELAY && (xTaskGetTickCount() - start) < wait_ticks)
        {
            if (eTaskGetState(ConfSRButtonTaskHandle_) == eDeleted)
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        ConfSRButtonTaskHandle_ = nullptr;
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
