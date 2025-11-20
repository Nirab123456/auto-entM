#include "headers/ReciverConfig.h"

static const char *bcfgTAG = "ARS_ReciverConfig_buttoncfg";

TaskHandle_t ReciverConfig::ConfSRButtonTaskHandle_ = nullptr;

//ISR
void IRAM_ATTR ReciverConfig::confButtonIsrHandle(void* arg)
{
    ReciverConfig* self = static_cast<ReciverConfig*>(arg);
    if (!self)
    {
        return;
    }
    
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
        if(ap_ssid && ap_ssid[0])
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
    
    if (!ok) {
        ESP_LOGE(bcfgTAG, "ReciverConfig::GSVIpPort:: portal failed or timed out");
        return false;
    }

    const char* entered_ip = ip_param.getValue();
    const char* entered_port = port_param.getValue();

    if (
        ((entered_ip == nullptr) || (entered_ip[0] == 0)) &&
        ((entered_port == nullptr) || (entered_port[0] == 0))
    )
    {
        ESP_LOGI(bcfgTAG, "ReciverConfig::GSVIpPort:: no IP/port entered in portal");
        return true;         
    }
    
    uint16_t port_val = 0;
    if (entered_port && entered_port[0])
    {
        long p = atol(entered_port);
        if (p > 0 && p <= 65525)
        {
            port_val = static_cast<uint16_t>(p);
        }
        else
        {
            port_val = 0;
        }
    }
    
    if (entered_ip && entered_ip[0])
    {
        IPAddress tmp;
        if (tmp.fromString(String(entered_ip)))
        {
            save(entered_ip, port_val);
            ESP_LOGI(bcfgTAG, "ReciverConfig::GSVIpPort - saved IP=%s PORT=%u\n", entered_ip, (unsigned)port_val);
            return true;
        }
        else
        {
            ESP_LOGE(bcfgTAG, "ReciverConfig::GSVIpPort - invalid IP entered (%s)\n", entered_ip);
            // do not save invalid IP; keep previous
            return false;
        }
    }
    else if (port_val != 0)
    {
        entered_ip = nullptr;
        save(entered_ip, port_val); // our save accepts nullptr for ip
        ESP_LOGI(bcfgTAG, "ReciverConfig::GSVIpPort - saved PORT=%u (IP unchanged)\n", (unsigned)port_val);
        return true;    
    }
    return true;
}

bool ReciverConfig::StartConfigPortal(bool force_start_conf_portal, const char* ap_ssid, const char* ap_password)
{
    ESP_LOGI(bcfgTAG, "ReciverConfig::startConfigPortal: preparing to start portal");
    bool was_paused = false;

    if (audio_rs_class_ptr_)
    {
        ESP_LOGD(bcfgTAG, "ReciverConfig::StartConfigPortal->PauseNetworkStreaming:Pausing audio");
        audio_rs_class_ptr_->PauseNetworkStreaming();
    }
    

    char ip_buffer[DEFAULT_IP_BUFFER_SIZE] = {0};
    char port_buffer[DEFAULT_PORT_BUFFER_SIZE] = {0};

    bool ok = GSVIpPort(
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
        String SavedIP = prefs_.getString(PREFS_IP_ID,"");
        String SavedPort = prefs_.getString(PREFS_PORT_ID,"");
        prefs_.end();
    }

    if (ok) {
        ESP_LOGI(bcfgTAG, "ReciverConfig::startConfigPortal: portal returned success (credentials acquired or already connected)");
    } else {
        ESP_LOGE(bcfgTAG, "ReciverConfig::startConfigPortal: portal failed or timed out");
    }
    
    if (audio_rs_class_ptr_)
    {
        bool reconnect_ok = audio_rs_class_ptr_ ->ReqNetworkReconnect();
        ESP_LOGD(bcfgTAG, "ReciverConfig::AUDIO_RS -> ReqNetworkReconnect:Returned %d\n", reconnect_ok ? 1 : 0);

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

// --- ConfButtonTaskLoop (fixed units & safe launch of portal) ---
void ReciverConfig::ConfButtonTaskLoop()
{
    TickType_t press_ticks = 0;
    TickType_t last_edge_tick = 0;
    uint8_t pin = prefs_rst_open_portal_pin_;
    const TickType_t debounce_ticks = debounce_ticks_;
    const TickType_t hold_ticks = pdMS_TO_TICKS(hold_ms_); // convert ms -> ticks

    for (;;) {
        // wait for ISR notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (stopping_.load(std::memory_order_acquire)) break;

        TickType_t now = xTaskGetTickCount();

        if ((now - last_edge_tick) < debounce_ticks) {
            last_edge_tick = now;
            continue;
        }
        last_edge_tick = now;

        int level = gpio_get_level(static_cast<gpio_num_t>(pin)); // prefer gpio_get_level for IDF
        if (level == 0) { // pressed (active low)
            if (press_ticks == 0) {
                press_ticks = now;
            }
            // wait for release event (task will be notified again on any edge)
            continue;
        } else { // released (level == 1)
            if (press_ticks == 0) {
                // spurious release without matching press
                continue;
            }
            TickType_t dur_ticks = now - press_ticks;
            press_ticks = 0;

            if (dur_ticks >= hold_ticks) {
                ESP_LOGI(bcfgTAG, "Button long press: %u ms", (unsigned)(dur_ticks * portTICK_PERIOD_MS));
                clear();

                // prefer launching the portal in a separate task to avoid blocking this task
                if (startConfigPortalCb_) {
                    // if callback launches portal in separate task that's ideal
                    startConfigPortalCb_();
                } else {
                    // launch StartConfigPortal in its own task
                    TaskHandle_t th = nullptr;
                    BaseType_t res = xTaskCreate(
                        ReciverConfig::StartConfPortalTrampoline,
                        "StartConfPortal",
                        8192,
                        this,
                        5,
                        &th
                    );
                    if (res != pdPASS) {
                        ESP_LOGE(bcfgTAG, "Failed to create StartConfPortal task");
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(500)); // allow system to settle
            } else {
                ESP_LOGI(bcfgTAG, "Short press (~%u ms) ignored", (unsigned)(dur_ticks * portTICK_PERIOD_MS));
            }
        }
    }

    DetachResetButton();
    vTaskDelete(nullptr);
}


bool ReciverConfig::configure_ConfRSTButton(uint8_t pin)
{
    // install ISR service once
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE) {
        // already installed - ok
    } else if (err != ESP_OK) {
        ESP_LOGE(bcfgTAG, "gpio_install_isr_service failed: %d", err);
        return false;
    }

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_ANYEDGE;   // <- any edge (both press and release)
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << static_cast<uint32_t>(pin));
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;

    esp_err_t r = gpio_config(&io_conf);
    if (r != ESP_OK) {
        ESP_LOGE(bcfgTAG, "gpio_config failed: %d", r);
        return false;
    }

    // Add ISR handler, pass 'this' as the arg
    esp_err_t e = gpio_isr_handler_add(static_cast<gpio_num_t>(pin), ReciverConfig::confButtonIsrHandle, this);
    if (e != ESP_OK) {
        ESP_LOGE(bcfgTAG, "gpio_isr_handler_add failed: %d", e);
        return false;
    }

    confRSTButton_configured_ = true;
    ESP_LOGI(bcfgTAG, "Attached ISR for pin %d", (int)pin);
    return true;
}


// --- AttachResetButton ---
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
    if (pin == 0xff) {
        return false;
    }

    // store pin so other functions can use it
    prefs_rst_open_portal_pin_ = pin;

    // create task if not already
    if (ConfSRButtonTaskHandle_ == nullptr) {
        BaseType_t ok = pdFAIL;
        if (!audio_rs_class_ptr_) {
            if (start_task_fn_ptr) {
                ok = start_task_fn_ptr() ? pdPASS : pdFAIL;
            } else {
                ESP_LOGE(bcfgTAG, "No start_task_fn_ptr and no audio_rs_class_ptr_ - cannot create ConfButtonTask");
                return false;
            }
        } else {
            ok = audio_rs_class_ptr_->start_task(
                "ConfButtonTaskLoop",
                task_stack_bytes,
                task_prio,
                core,
                ConfRstButtonTrampoline,
                this,
                &ConfSRButtonTaskHandle_
            ) ? pdPASS : pdFAIL;
        }
        if (ok != pdPASS || ConfSRButtonTaskHandle_ == nullptr) {
            ESP_LOGE(bcfgTAG, "ConfButtonTaskLoop: Failed creation");
            ConfSRButtonTaskHandle_ = nullptr;
            if (audio_rs_class_ptr_) audio_rs_class_ptr_->conf_portal_rst_button_handler_ = nullptr;
            return false;
        }
        if (audio_rs_class_ptr_) audio_rs_class_ptr_->conf_portal_rst_button_handler_ = ConfSRButtonTaskHandle_;
    }

    // configure gpio/ISR once
    if (!confRSTButton_configured_) {
        bool ok = configure_ConfRSTButton(pin);
        if (!ok) {
            ESP_LOGE(bcfgTAG, "configure_ConfRSTButton failed");
            return false;
        }
    } else {
        ESP_LOGI(bcfgTAG, "AttachResetButton: already configured");
    }

    return true;
}

// --- DetachResetButton (cleanup) ---
void ReciverConfig::DetachResetButton(TickType_t wait_ms)
{
    stopping_.store(true, std::memory_order_release);

    if (prefs_rst_open_portal_pin_ != 0xff) {
        // remove ISR handler added with gpio_isr_handler_add
        esp_err_t r = gpio_isr_handler_remove(static_cast<gpio_num_t>(prefs_rst_open_portal_pin_));
        if (r == ESP_OK) {
            ESP_LOGD(bcfgTAG, "gpio_isr_handler_remove succeeded");
        } else {
            ESP_LOGE(bcfgTAG, "gpio_isr_handler_remove failed: %d", r);
        }
    }

    if (ConfSRButtonTaskHandle_) {
        xTaskNotifyGive(ConfSRButtonTaskHandle_);
        const TickType_t start = xTaskGetTickCount();
        const TickType_t wait_ticks = wait_ms;
        while (wait_ms != portMAX_DELAY && (xTaskGetTickCount() - start) < wait_ticks) {
            if (eTaskGetState(ConfSRButtonTaskHandle_) == eDeleted) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        ConfSRButtonTaskHandle_ = nullptr;
        if (audio_rs_class_ptr_ && audio_rs_class_ptr_->conf_portal_rst_button_handler_) {
            audio_rs_class_ptr_->conf_portal_rst_button_handler_ = nullptr;
        }
    }
    stopping_.store(false, std::memory_order_release);
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
    self -> StartConfigPortal(true);
}
