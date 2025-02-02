//TODO bei websitereload downloaded manchmal iwas komisches
//TODO https Zertifikate sind ungültig

#include "webserver.h"
#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <esp_log.h>
#include <cJSON.h>
#include <esp_sleep.h>

// Dauerspeicher
#include "nvs_flash.h"
#include "nvs.h"



// Pin Definitionen
#define PAUSE_PIN           15
#define HUMAN_PIN1         18
#define MACHINE_PIN_IN1    22
#define MACHINE_PIN_OUT1   27
#define HUMAN_PIN2         19
#define MACHINE_PIN_IN2    23
#define MACHINE_PIN_OUT2   32
#define LED1_PIN           25
#define LED2_PIN           26
#define PAUSE_LED_PIN      13
#define POWER_LED_PIN      2

// Timing Konstanten
#define PULSE_DURATION           100
#define START_ANIMATION_FACTOR   5
#define SIGNAL_DELAY            500
#define MAX_TIME                300000

//Buttontiming
static bool p1_pressed = false;
static bool p2_pressed = false;

static uint64_t p1_press_start_time = 0;
static uint64_t p2_press_start_time = 0;

#define BOTH_HELD_DURATION 1000 // Dauer in Millisekunden für gleichzeitiges Halten
#define MAX_TIME_DIFFERENCE 500 // Maximal erlaubte Zeitdifferenz in Millisekunden


static uint64_t edit_time_press_duration = 0;
static bool edit_time = false;



// Globale Variablen
uint32_t max_time = MAX_TIME;
uint32_t time_player_1 = MAX_TIME;
uint32_t time_player_2 = MAX_TIME;
static uint64_t last_millis = 0;
static uint64_t signal_delay_start_time = 0;

// Globale Variable für den ISR Status
static volatile uint8_t button_event = 0;  // 0 = kein Event, 1 = Player1, 2 = Player2
static volatile uint8_t edit_time_event = 0;  // 0 = kein Event, 1 = -30s, 2 = +30s, 3 = both pressed 5 = pressdelay

// Globale Variable für den Spielstatus
static bool game_has_started = false;

// Globale Variable für den Pause-Status
static volatile uint8_t pause_event = 0;  // 0 = kein Event, 1 = Resume, 2 = Pause

bool is_game_paused = false;
bool its_player1s_turn = false;
static bool is_in_signal_delay = false;
static bool need_to_send_signal = false;

//server
httpd_handle_t server_handle = NULL;



#define LONG_PRESS_TIME 3000 // 3 Sekunden


void shutdown_esp() {
  // Stoppen Sie alle laufenden Prozesse
  if (server_handle != NULL) {
    httpd_stop(server_handle);
  }
  
  
  
  // Konfigurieren Sie den Aufwach-Mechanismus
  esp_sleep_enable_ext0_wakeup(PAUSE_PIN, 0); // Aufwachen bei LOW-Signal
  
  // Gehen Sie in den Deep-Sleep-Modus
  esp_deep_sleep_start();
}

void check_long_press() {
  static uint64_t press_start = 0;
  if (gpio_get_level(PAUSE_PIN) == 0) { // Taste gedrückt
    if (press_start == 0) {
      press_start = esp_timer_get_time() / 1000;
    } else if ((esp_timer_get_time() / 1000) - press_start > LONG_PRESS_TIME) {
        // Langer Tastendruck erkannt

        // Schalten Sie die Power-LED aus
        gpio_set_level(POWER_LED_PIN, 0);
        while (gpio_get_level(PAUSE_PIN) == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        shutdown_esp();
        
    }
  } else {
    press_start = 0;
  }
}




static void update_leds(void) {
    gpio_set_level(LED1_PIN, its_player1s_turn);
    gpio_set_level(LED2_PIN, !its_player1s_turn);
}

static void next_player(void) {
    printf("switching player\n");
    its_player1s_turn = !its_player1s_turn;
    update_leds();
}

//SIGNAL
static void sendSignal() {
    if (!is_game_paused) {
        gpio_set_level(PAUSE_LED_PIN, 0);
        next_player();
        if (its_player1s_turn) {
            gpio_set_level(MACHINE_PIN_OUT1, 1);
            vTaskDelay(pdMS_TO_TICKS(PULSE_DURATION));
            gpio_set_level(MACHINE_PIN_OUT1, 0);
        } else {
            gpio_set_level(MACHINE_PIN_OUT2, 1);
            vTaskDelay(pdMS_TO_TICKS(PULSE_DURATION));
            gpio_set_level(MACHINE_PIN_OUT2, 0);
        }
        need_to_send_signal = false;
        is_in_signal_delay = false;


        // JSON-Nachricht erstellen
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "action", "start_time");
        cJSON_AddNumberToObject(json, "current_player", its_player1s_turn ? 1 : 2);

        // JSON-Nachricht senden
        if (server_handle != NULL) {
            char *json_str = cJSON_Print(json);
            send_json_to_clients(server_handle, json_str);

            // Speicher freigeben
            free(json_str);
        }
        else {
            ESP_LOGE("main", "Server-Handle ist NULL!");
        }
        cJSON_Delete(json);
    }
} 


static void resume_game() {
    
    gpio_set_level(LED1_PIN, 0);
    gpio_set_level(LED2_PIN, 0);
    // Pause-LED Blinkt 3x bevor es weiter geht
    for (int i = 0; i < 6; i++)
    {
        
        if (its_player1s_turn)
        {
            gpio_set_level(LED2_PIN, (i)%2);
        }
        else {
            gpio_set_level(LED1_PIN, (i)%2);
        }
        
        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    gpio_set_level(PAUSE_LED_PIN, 0);
    last_millis = esp_timer_get_time() / 1000;
    is_game_paused = false;
    printf("GameResumed! \n");
    sendSignal();
}

static void pause_game() {
    printf("GamePaused! \n");
    is_game_paused = true;
    gpio_set_level(PAUSE_LED_PIN, 1);
    gpio_set_level(LED1_PIN, !its_player1s_turn);
    gpio_set_level(LED2_PIN, its_player1s_turn);

    // JSON-Nachricht erstellen
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "action", "pause_time");
    cJSON_AddNumberToObject(json, "player1_time", time_player_1);
    cJSON_AddNumberToObject(json, "player2_time", time_player_2);
    cJSON_AddNumberToObject(json, "current_player", its_player1s_turn ? 1 : 2);

    // JSON-Nachricht senden
    if (server_handle != NULL)
    {
        char *json_str = cJSON_Print(json);
        send_json_to_clients(server_handle, json_str);

        // Speicher freigeben
        free(json_str);
    }
    else {
        ESP_LOGE("main", "Server-Handle ist NULL!");
    }
    cJSON_Delete(json);
}  




static void start_signal_delay() {
    is_in_signal_delay = true;
    need_to_send_signal = true;
    signal_delay_start_time = esp_timer_get_time() / 1000;
    gpio_set_level(PAUSE_LED_PIN, 1);
    if (pause_event == 2) {
        pause_game();
        pause_event = 0;
    }
    else {
        vTaskDelay(pdMS_TO_TICKS(SIGNAL_DELAY));
        sendSignal();
    }

    
    
}

static void handle_signal(int player) {
    if (!is_game_paused)
    {
        // warte auf Senden des Signals
        if (!need_to_send_signal){ // damit es nicht resetet wird bei mehrfachem auslösen
            if ((its_player1s_turn && player == 1) || (!its_player1s_turn && player == 2)) // es wird nur auf Signale des aktuellen Spielers gehört
            {
                start_signal_delay();
            }            
        }
    }
    
    
}

// ISR Handler
static void IRAM_ATTR handle_signal_player1_isr(void* arg) {
    if (!is_game_paused)
    {
        button_event = 1;
    }

    if (is_game_paused && !game_has_started && edit_time_event != 5)
    {
        uint64_t now = esp_timer_get_time() / 1000;
        if (edit_time_event == 0)
        {
            edit_time = false;
            edit_time_event = 1;
        }
        if (!p1_pressed) {
            p1_pressed = true;
            p1_press_start_time = now;
        }
    }

    
}

static void IRAM_ATTR handle_signal_player2_isr(void* arg) {
    if (!is_game_paused)
    {
        button_event = 2;
    }

    if (is_game_paused && !game_has_started  && edit_time_event != 5)
    {
        uint64_t now = esp_timer_get_time() / 1000;
        if (edit_time_event == 0)
        {
            edit_time = false;
            edit_time_event = 2;
        }
        if (!p2_pressed) {
            
            p2_pressed = true;
            p2_press_start_time = now;
        }
    } 
}


static void handle_edit_time(int event) {
    if (event == 1)
    {
        if (max_time > 30000)
        {
            max_time -= 30000;
        }
        
    }
    else if (event == 2)
    {
        max_time += 30000;
        
    }

    
    time_player_1 = max_time;
    time_player_2 = max_time;

    // JSON-Nachricht erstellen
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "action", "init_time");
    cJSON_AddNumberToObject(json, "max_time", max_time);
    cJSON_AddNumberToObject(json, "current_player", its_player1s_turn ? 1 : 2);

    // JSON-Nachricht senden
    if (server_handle != NULL)
    {
        char *json_str = cJSON_Print(json);
        send_json_to_clients(server_handle, json_str);

        // Speicher freigeben
        free(json_str);
    }
    else {
        ESP_LOGE("main", "Server-Handle ist NULL!");
    }
    cJSON_Delete(json);
}

  


static void IRAM_ATTR pause_game_isr(void* arg) {
    if (is_game_paused) {
        pause_event = 1;  // Resume requested
    }
    else {
        pause_event = 2;  // Pause requested
    }
    
    
}







static void update_time(void) {
    if (!is_game_paused && !is_in_signal_delay) {
        uint64_t current_millis = esp_timer_get_time() / 1000;
        uint64_t delta_time = current_millis - last_millis;
        
        if (its_player1s_turn && time_player_1 > 0) {
            time_player_1 -= delta_time;
        } else if (!its_player1s_turn && time_player_2 > 0) {
            time_player_2 -= delta_time;
        }
        
        last_millis = current_millis;
    }
}

static void end_game() {
    is_game_paused = true;

    if (its_player1s_turn) {
        time_player_1 = 0;
    } else {
        time_player_2 = 0;
    }

    while (1)
    {
        gpio_set_level(LED1_PIN, its_player1s_turn);
        gpio_set_level(LED2_PIN, !its_player1s_turn);
        vTaskDelay(pdMS_TO_TICKS(500));
        
        gpio_set_level(LED1_PIN, 0);
        gpio_set_level(LED2_PIN, 0);

        check_long_press(); // Überprüfen Sie auf langen Tastendruck
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
}

static void time_is_up() {
    if (time_player_1 <= 0) {
        time_player_1 = 0;
        gpio_set_level(LED1_PIN, 1);
        gpio_set_level(LED2_PIN, 0);
    } else {
        time_player_2 = 0;
        gpio_set_level(LED1_PIN, 0);
        gpio_set_level(LED2_PIN, 1);
    }
    end_game();
}


// Initialisierung des NVS-Speichers
esp_err_t init_nvs() {
    printf("init_nvs\n");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    return ret;
}

// Zum Speichern von Daten
void save_max_time(uint32_t max_time) {
    printf("save_max_time\n");
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        // Fehlerbehandlung
        printf("Fehler bei save_max_time nvs_open\n");
        return;
    }
    err = nvs_set_u32(my_handle, "max_time", max_time);
    if (err != ESP_OK) {
        // Fehlerbehandlung
        printf("Fehler bei save_max_time nvs_set_u32\n");
    }
    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        // Fehlerbehandlung
        printf("Fehler bei save_max_time nvs_commit\n");
    }
    nvs_close(my_handle);
}

// Zum Lesen von Daten
uint32_t load_max_time(uint32_t default_value) {
    printf("load_max_time\n");
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        printf("Fehler bei load_max_time nvs_open\n");
        return default_value;
    }
    uint32_t max_time;
    err = nvs_get_u32(my_handle, "max_time", &max_time);
    if (err != ESP_OK) {
        printf("Fehler bei load_max_time nvs_get_u32\n");
        max_time = default_value;
    }
    nvs_close(my_handle);
    return max_time;
}



void app_main(void) {
    // Preferences aus Speicher initialisieren
    init_nvs();
    max_time = load_max_time(max_time);
    time_player_1 = max_time;
    time_player_2 = max_time;

    // WebServer initialisieren
    init_webserver();
    
    
    while (server_handle == NULL)
    {        
        server_handle = get_webserver_handle();
        if (server_handle == NULL)
        {
            ESP_LOGE("main", "Server-Handle ist NULL!");
        }
    }
    ESP_LOGI("main", "Server-Handle erhalten!");


    // GPIO Konfiguration
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    
    // Output Pins konfigurieren
    io_conf.pin_bit_mask = (1ULL<<LED1_PIN) | (1ULL<<LED2_PIN) | 
                          (1ULL<<PAUSE_LED_PIN) | (1ULL<<POWER_LED_PIN) |
                          (1ULL<<MACHINE_PIN_OUT1) | (1ULL<<MACHINE_PIN_OUT2);
    gpio_config(&io_conf);

    // Input Pins konfigurieren
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pin_bit_mask = (1ULL<<PAUSE_PIN);
    io_conf.intr_type = GPIO_INTR_POSEDGE; // Steigende Flanke

    gpio_config(&io_conf);

    gpio_config_t io_conf_humane = {
        .pin_bit_mask = (1ULL << HUMAN_PIN1) | (1ULL << HUMAN_PIN2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // Pull-Up aktivieren
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE // Fallende Flanke
    };
    gpio_config(&io_conf_humane);

    

    // Mashine-Input Pins konfigurieren
    gpio_config_t io_conf_machine = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  // Pull-down aktivieren
        .intr_type = GPIO_INTR_POSEDGE,        // Trigger bei steigender Flanke (0V -> 3.3V)
        .pin_bit_mask = (1ULL<<MACHINE_PIN_IN1) | (1ULL<<MACHINE_PIN_IN2)
    };


    gpio_install_isr_service(0);

    gpio_isr_handler_add(PAUSE_PIN, pause_game_isr, NULL);
    gpio_isr_handler_add(HUMAN_PIN1, handle_signal_player1_isr, NULL);
    gpio_isr_handler_add(MACHINE_PIN_IN1, handle_signal_player1_isr, NULL);
    gpio_isr_handler_add(HUMAN_PIN2, handle_signal_player2_isr, NULL);
    gpio_isr_handler_add(MACHINE_PIN_IN2, handle_signal_player2_isr, NULL);



    // Initialisierung
    gpio_set_level(POWER_LED_PIN, 1);
    its_player1s_turn = esp_random() & 1; // Zufälliger Startspieler

    for(int i = 30; i > 0; i--) {
        gpio_set_level(LED1_PIN, 1);
        gpio_set_level(LED2_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS((i+0.5)*START_ANIMATION_FACTOR));
        gpio_set_level(LED1_PIN, 0);
        gpio_set_level(LED2_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(i*START_ANIMATION_FACTOR));
    }

    update_leds();

    pause_game();// waiting for pauseputton to be pressed

    while (is_game_paused)
    {
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
        if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
            // Warten Sie auf das Loslassen der Taste
            while (gpio_get_level(PAUSE_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        check_long_press(); // Überprüfen Sie auf langen Tastendruck
        // start game
        if (pause_event == 1) {
            
            // Save Preferences der maxTime im Speicher falls sie sich geändert hat
            if (load_max_time(max_time) != max_time) {
                save_max_time(max_time);
            }

            resume_game();
            pause_event = 0; // Event zurücksetzen
        }



        uint64_t now = esp_timer_get_time() / 1000;

        // edit maxtime
        if (edit_time && edit_time_event != 0 && edit_time_event != 5) {
            if (edit_time_press_duration < 500 && edit_time_press_duration > 10)
            {
                printf("hm\n");
                handle_edit_time(edit_time_event);
                edit_time_event = 5; // delay for not doublepressing
                vTaskDelay(pdMS_TO_TICKS(250));
                edit_time_event = 0; // Event zurücksetzen
                edit_time = false;
            }
            else {
                edit_time_event = 0; // Event zurücksetzen
                edit_time = false;
            }
            
            
        }
        

        
        // switch startplayer
        if (p1_pressed && p2_pressed) {
            if (abs(p1_press_start_time - p2_press_start_time) < MAX_TIME_DIFFERENCE)
            {
                if (now - p1_press_start_time > BOTH_HELD_DURATION)
                {
                    
                    next_player();
                    handle_edit_time(0); // switch currentplayer on display
                    p1_pressed = false;
                    p2_pressed = false;
                    
                    
                }
                
            }
            else {
                printf("Both NOT pressed at the same time!\n");
            }
            
        }
        

        // reset press after release
        if (p1_pressed && gpio_get_level(HUMAN_PIN1) == 1) {
            p1_pressed = false;
            edit_time = true;
            edit_time_press_duration = now - p1_press_start_time;
        }
        // reset press after release
        if (p2_pressed && gpio_get_level(HUMAN_PIN2) == 1) {
            p2_pressed = false;
            edit_time = true;
            edit_time_press_duration = now - p2_press_start_time;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    
    

    game_has_started = true;
    while(1) {
        // Handle resume after pause
        if (pause_event == 1) {
            resume_game();
            pause_event = 0; // Event zurücksetzen
        }

        
        // Handle buttonEvent
        if (button_event != 0) {
            handle_signal(button_event);
            button_event = 0;  // Event zurücksetzen
        }
        
        
        update_time();
        if (time_player_1 <= 0 || time_player_2 <= 0)
        {
            time_is_up();
        }
        
        
        check_long_press(); // Überprüfen Sie auf langen Tastendruck

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

