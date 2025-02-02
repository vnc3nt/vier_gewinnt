#include "webserver.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_wifi.h"
#include "protocol_examples_common.h"
#include "lwip/sockets.h"
#include <esp_https_server.h>
#include "keep_alive.h"
#include "sdkconfig.h"
#include "mdns.h"
#include "global_vars.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"



// Definiere ein Event-Bit, das anzeigt, dass eine Verbindung hergestellt wurde
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI("WIFI_STA", "Verbindung getrennt. Erneuter Verbindungsversuch...");
                esp_wifi_connect();
                xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI("WIFI_STA", "Erhaltene IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    // Erstelle die Event-Gruppe
    s_wifi_event_group = xEventGroupCreate();

    // Initialisiere netif und das Standard-Event-Loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Erstelle ein Standard-WiFi-STA-Interface
    esp_netif_create_default_wifi_sta();
    
    // Initialisiere die WiFi-Konfiguration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Registriere Event-Handler für WiFi- und IP-Events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    
    // Konfiguriere den STA-Modus mit SSID und Passwort
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "FRITZ!Box Gastzugang KF",
            .password = "IOE8N9PeUTiH7FQIG8yx",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // Setze den WiFi-Modus auf STA und wende die Konfiguration an
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // Starte das WiFi
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI("WIFI_STA", "WiFi im STA-Modus gestartet. Versuche Verbindung zu SSID: %s", "FRITZ!Box Gastzugang KF");
    
    // Warte blockierend, bis das Event "IP erhalten" gesetzt wurde
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, portMAX_DELAY);
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI("WIFI_STA", "Verbindung erfolgreich hergestellt.");
    }
}




//gloabl server
static httpd_handle_t server = NULL;

#if !CONFIG_HTTPD_WS_SUPPORT
#error This example cannot be used unless HTTPD_WS_SUPPORT is enabled in esp-http-server component configuration
#endif

struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

static const char *TAG = "wss_echo_server";
static const size_t max_clients = 4;

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // First receive the full ws message
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
    }
    // If it was a PONG, update the keep-alive
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        ESP_LOGD(TAG, "Received PONG message");
        free(buf);
        return wss_keep_alive_client_is_active(httpd_get_global_user_ctx(req->handle),
                httpd_req_to_sockfd(req));

    // If it was a TEXT message, just echo it back
    } else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_PING || ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            ESP_LOGI(TAG, "Received packet with message: %s", ws_pkt.payload);
        } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
            // Response PONG packet to peer
            ESP_LOGI(TAG, "Got a WS PING frame, Replying PONG");
            ws_pkt.type = HTTPD_WS_TYPE_PONG;
        } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            // Response CLOSE packet with no payload to peer
            ws_pkt.len = 0;
            ws_pkt.payload = NULL;
        }
        ret = httpd_ws_send_frame(req, &ws_pkt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
        }
        ESP_LOGI(TAG, "ws_handler: httpd_handle_t=%p, sockfd=%d, client_info:%d", req->handle,
                 httpd_req_to_sockfd(req), httpd_ws_get_fd_info(req->handle, httpd_req_to_sockfd(req)));
        free(buf);
        return ret;
    }
    free(buf);
    return ESP_OK;
}

esp_err_t wss_open_fd(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "New client connected %d", sockfd);
    wss_keep_alive_t h = httpd_get_global_user_ctx(hd);
    return wss_keep_alive_add_client(h, sockfd);
}

void wss_close_fd(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "Client disconnected %d", sockfd);
    wss_keep_alive_t h = httpd_get_global_user_ctx(hd);
    wss_keep_alive_remove_client(h, sockfd);
    close(sockfd);
}

static const httpd_uri_t ws = {
        .uri        = "/ws",
        .method     = HTTP_GET,
        .handler    = ws_handler,
        .user_ctx   = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true
};


static void send_hello(void *arg)
{
    static const char * data = "Hello client :)";
    struct async_resp_arg *resp_arg = arg;
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)data;
    ws_pkt.len = strlen(data);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    free(resp_arg);
}

static void send_ping(void *arg)
{
    struct async_resp_arg *resp_arg = arg;
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = NULL;
    ws_pkt.len = 0;
    ws_pkt.type = HTTPD_WS_TYPE_PING;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    free(resp_arg);
}

bool client_not_alive_cb(wss_keep_alive_t h, int fd)
{
    ESP_LOGE(TAG, "Client not alive, closing fd %d :(", fd);
    httpd_sess_trigger_close(wss_keep_alive_get_user_ctx(h), fd);
    return true;
}

bool check_client_alive_cb(wss_keep_alive_t h, int fd)
{
    ESP_LOGD(TAG, "Checking if client (fd=%d) is alive", fd);
    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
    assert(resp_arg != NULL);
    resp_arg->hd = wss_keep_alive_get_user_ctx(h);
    resp_arg->fd = fd;

    if (httpd_queue_work(resp_arg->hd, send_ping, resp_arg) == ESP_OK) {
        return true;
    }
    return false;
}


// Handler for index.html
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[] asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);
    return httpd_resp_send(req, (const char *)index_html_start, index_html_size);
}

// Handler for style.css
static esp_err_t style_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/css");
    extern const unsigned char style_css_start[] asm("_binary_style_css_start");
    extern const unsigned char style_css_end[] asm("_binary_style_css_end");
    const size_t style_css_size = (style_css_end - style_css_start);
    return httpd_resp_send(req, (const char *)style_css_start, style_css_size);
}

// Handler for script.js
static esp_err_t script_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/javascript");
    extern const unsigned char script_js_start[] asm("_binary_script_js_start");
    extern const unsigned char script_js_end[] asm("_binary_script_js_end");
    const size_t script_js_size = (script_js_end - script_js_start);
    return httpd_resp_send(req, (const char *)script_js_start, script_js_size);
}

// Handler für sun-moon.png
static esp_err_t sun_moon_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/png");
    extern const unsigned char img_sun_moon_png_start[] asm("_binary_sun_moon_png_start");
    extern const unsigned char img_sun_moon_png_end[] asm("_binary_sun_moon_png_end");
    const size_t img_sun_moon_png_size = (img_sun_moon_png_end - img_sun_moon_png_start);
    return httpd_resp_send(req, (const char *)img_sun_moon_png_start, img_sun_moon_png_size);
}


static httpd_handle_t start_wss_echo_server(void)
{
    // Prepare keep-alive engine
    wss_keep_alive_config_t keep_alive_config = KEEP_ALIVE_CONFIG_DEFAULT();
    keep_alive_config.max_clients = max_clients;
    keep_alive_config.client_not_alive_cb = client_not_alive_cb;
    keep_alive_config.check_client_alive_cb = check_client_alive_cb;
    wss_keep_alive_t keep_alive = wss_keep_alive_start(&keep_alive_config);

    // Start the httpd server
    httpd_handle_t server = NULL;
    ESP_LOGI(TAG, "Starting server");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.max_open_sockets = max_clients;
    conf.httpd.global_user_ctx = keep_alive;
    conf.httpd.open_fn = wss_open_fd;
    conf.httpd.close_fn = wss_close_fd;

    extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
    extern const unsigned char servercert_end[]   asm("_binary_servercert_pem_end");
    conf.servercert = servercert_start;
    conf.servercert_len = servercert_end - servercert_start;

    extern const unsigned char prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
    extern const unsigned char prvtkey_pem_end[]   asm("_binary_prvtkey_pem_end");
    conf.prvtkey_pem = prvtkey_pem_start;
    conf.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;

    esp_err_t ret = httpd_ssl_start(&server, &conf);
    if (ESP_OK != ret) {
        ESP_LOGI(TAG, "Error starting server!");
        return NULL;
    }

    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    httpd_register_uri_handler(server, &ws);
    wss_keep_alive_set_user_ctx(keep_alive, server);


    // URI Handler Configuration
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL
        };
        httpd_uri_t style_uri = {
            .uri = "/style.css",
            .method = HTTP_GET,
            .handler = style_handler,
            .user_ctx = NULL
        };
        httpd_uri_t script_uri = {
            .uri = "/script.js",
            .method = HTTP_GET,
            .handler = script_handler,
            .user_ctx = NULL
        };
        httpd_uri_t sun_moon_uri = {
            .uri = "/img/sun-moon.png",
            .method = HTTP_GET,
            .handler = sun_moon_handler,
            .user_ctx = NULL
        };

        

    // Register handlers
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &style_uri);
    httpd_register_uri_handler(server, &script_uri);
    httpd_register_uri_handler(server, &sun_moon_uri);


    ESP_LOGI(TAG, "WebSocket server started successfully");

    if (server != NULL) {
        ESP_LOGI(TAG, "server != NULL");
    }
    else {
        ESP_LOGE(TAG, "server == NULL");
    }
    
    return server;
}

static esp_err_t stop_wss_echo_server(httpd_handle_t server)
{
    // Stop keep alive thread
    wss_keep_alive_stop(httpd_get_global_user_ctx(server));
    // Stop the httpd server
    return httpd_ssl_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        if (stop_wss_echo_server(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop https server");
        }
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        *server = start_wss_echo_server();
    }
}

// Get all clients and send async message
static void wss_server_send_messages_task(void* pvParameters) {
    httpd_handle_t* server = (httpd_handle_t*)pvParameters;
    bool send_messages = true;

    while (send_messages) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
        if (!*server) {
            continue;
        }
        
        size_t clients = max_clients;
        int client_fds[max_clients];
        
        if (httpd_get_client_list(*server, &clients, client_fds) == ESP_OK) {
            for (size_t i=0; i < clients; ++i) {
                int sock = client_fds[i];
                if (httpd_ws_get_fd_info(*server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
                    ESP_LOGI(TAG, "Active client (fd=%d) -> sending async message", sock);
                    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
                    assert(resp_arg != NULL);
                    resp_arg->hd = *server;
                    resp_arg->fd = sock;
                    if (httpd_queue_work(resp_arg->hd, send_hello, resp_arg) != ESP_OK) {
                        ESP_LOGE(TAG, "httpd_queue_work failed!");
                        send_messages = false;
                        break;
                    }
                }
            }
        }
    }
    vTaskDelete(NULL);
}







static esp_err_t time_handler(httpd_req_t *req) {
    char response[128];
    snprintf(response, sizeof(response), 
         "{\"max_time\": %lu, \"time_player_1\": %lu, \"time_player_2\": %lu, \"is_paused\": \"%s\", \"its_player1s_turn\": %d}",
         max_time, time_player_1, time_player_2, is_game_paused ? "true" : "false", its_player1s_turn ? 1 : 2);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, strlen(response));
}

void register_time_handler(httpd_handle_t server) {
    httpd_uri_t time_uri = {
        .uri       = "/time",
        .method    = HTTP_GET,
        .handler   = time_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &time_uri);
}

void init_webserver(void) {
    
    ESP_LOGI(TAG, "Webserver code is starting...");
    ESP_LOGI(TAG, "webserver startet");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize WiFi 
    wifi_init_sta();

    // Start the WSS Server
    server = start_wss_echo_server();
    if (server == NULL)
    {
        ESP_LOGE(TAG, "Fehler beim Starten des WebSocket-Servers!");
        return;
    }
    
    
    // Start in a separate task
    xTaskCreate(wss_server_send_messages_task, "wss_messages", 2048, &server, 5, NULL); // Adjusted stack size

    register_time_handler(server);

}




// Funktion zum Senden einer JSON-Nachricht an alle verbundenen WebSocket-Clients
void send_json_to_clients(httpd_handle_t server, const char *json_str) {
    if (server == NULL) {
        ESP_LOGE(TAG, "WebSocket server handle is NULL");
        return;
    }

    size_t clients = max_clients; // Maximale Anzahl von Clients
    int client_fds[max_clients];  // Array für Client-Sockets

    // Hole die Liste der verbundenen Clients
    if (httpd_get_client_list(server, &clients, client_fds) == ESP_OK) {
        for (size_t i = 0; i < clients; ++i) {
            int sock = client_fds[i];

            // Prüfe, ob der Client ein aktiver WebSocket ist
            if (httpd_ws_get_fd_info(server, sock) == HTTPD_WS_CLIENT_WEBSOCKET) {
                ESP_LOGI(TAG, "Sende Nachricht an Client (fd=%d): %s", sock, json_str);

                httpd_ws_frame_t ws_pkt = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)json_str,
                    .len = strlen(json_str),
                };

                // Sende die Nachricht asynchron an den Client
                esp_err_t ret = httpd_ws_send_frame_async(server, sock, &ws_pkt);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Fehler beim Senden an Client (fd=%d): %s", sock, esp_err_to_name(ret));
                }
            }
        }
    } else {
        ESP_LOGE(TAG, "Fehler beim Abrufen der Client-Liste");
    }
}

httpd_handle_t get_webserver_handle(void) {
    return server;
}