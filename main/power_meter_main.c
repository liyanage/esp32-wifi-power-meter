#include <stdio.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// console support (config menu)
#include "linenoise/linenoise.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"


#include <esp_http_server.h>

#include <driver/adc.h>

typedef enum configuration_state {
    configuration_state_start,
    configuration_state_error,
    configuration_state_check_for_nvs_configuration_data,
    configuration_state_waiting_for_menu_choice,
    configuration_state_restarting,
    configuration_state_querying_for_config_menu,
    configuration_state_starting_config_menu,
    configuration_state_running_config_menu,
    configuration_state_querying_for_wifi_credentials,
    configuration_state_running_calibration_menu,
    configuration_state_querying_for_calibration_y_intercept,
    configuration_state_querying_for_calibration_slope,
    configuration_state_success
} configuration_state;

#define ENUM_TO_STRING_CASE(x) case x: return #x
char *configuration_state_label_for_value(configuration_state state);
char *configuration_state_label_for_value(configuration_state state) {
    switch (state) {
        ENUM_TO_STRING_CASE(configuration_state_start);
        ENUM_TO_STRING_CASE(configuration_state_error);
        ENUM_TO_STRING_CASE(configuration_state_check_for_nvs_configuration_data);
        ENUM_TO_STRING_CASE(configuration_state_waiting_for_menu_choice);
        ENUM_TO_STRING_CASE(configuration_state_restarting);
        ENUM_TO_STRING_CASE(configuration_state_querying_for_config_menu);
        ENUM_TO_STRING_CASE(configuration_state_starting_config_menu);
        ENUM_TO_STRING_CASE(configuration_state_running_config_menu);
        ENUM_TO_STRING_CASE(configuration_state_querying_for_wifi_credentials);
        ENUM_TO_STRING_CASE(configuration_state_running_calibration_menu);
        ENUM_TO_STRING_CASE(configuration_state_querying_for_calibration_y_intercept);
        ENUM_TO_STRING_CASE(configuration_state_querying_for_calibration_slope);
        ENUM_TO_STRING_CASE(configuration_state_success);
    }
    return NULL;
}

#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASSWORD "wifi_password"
#define NVS_KEY_ADC_CALIBRATION_Y_INTERCEPT "adc_intercept"
#define NVS_KEY_ADC_CALIBRATION_SLOPE "adc_slope"
#define WIFI_CREDENTIAL_BUFFER_SIZE 80
#define CONFIGURATION_MENU_TIMEOUT_SECONDS 2

static char s_wifi_ssid[WIFI_CREDENTIAL_BUFFER_SIZE];
static char s_wifi_password[WIFI_CREDENTIAL_BUFFER_SIZE];
static float s_adc_calibration_y_intercept = 1.35;
static float s_adc_calibration_slope = 37.32;

bool run_configuration_menu_state_machine(void);
bool open_nvs_handle(nvs_handle *handle);
void configuration_transition_to_state(configuration_state *current_state, configuration_state new_state);
float averaged_adc_sample(void);
float apply_calibration_to_adc_sample(float sample);
bool query_float_value(char *prompt, float *out_value);
bool read_nvs_config_calibration_data(nvs_handle my_handle);
bool read_nvs_config_wifi_credentials(nvs_handle my_handle);
bool write_nvs_config_calibration_data(void);


float averaged_adc_sample(void) {
    long sum = 0;
    int count = 20;
    for (int i = 0; i < count; i++) {
        int raw = adc1_get_raw(ADC1_CHANNEL_6);
        sum += raw;
        vTaskDelay(15 / portTICK_PERIOD_MS);
    }
    return ((float)sum / (float)count) / 4095.0;
}

float apply_calibration_to_adc_sample(float sample) {
    return s_adc_calibration_y_intercept + s_adc_calibration_slope * sample;
}

esp_err_t http_handler_power_status_get(httpd_req_t *req) {
    float sample = averaged_adc_sample();
    float voltage = apply_calibration_to_adc_sample(sample);
    
    httpd_resp_set_type(req, "text/plain");
    char buffer[80];
    snprintf(buffer, sizeof(buffer), "voltage %.2f\n", voltage);
    httpd_resp_send(req, buffer, strlen(buffer));
    return ESP_OK;
}

httpd_uri_t uri_get = {
    .uri      = "/power_status",
    .method   = HTTP_GET,
    .handler  = http_handler_power_status_get,
    .user_ctx = NULL
};

httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_get);
    }

    return server;
}

void stop_webserver(httpd_handle_t server) {
    httpd_stop(server);
}

esp_err_t event_handler(void *ctx, system_event_t *event) {
    printf("Event handler\n");
    httpd_handle_t *server = (httpd_handle_t *) ctx;

    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        printf("SYSTEM_EVENT_STA_START\n");
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        printf("SYSTEM_EVENT_STA_GOT_IP\n");
        printf("Got IP: '%s'\n", ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));

        if (*server == NULL) {
            *server = start_webserver();
        }
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        printf("SYSTEM_EVENT_STA_DISCONNECTED\n");
        ESP_ERROR_CHECK(esp_wifi_connect());

        if (*server) {
            stop_webserver(*server);
            *server = NULL;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}


void app_main() {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);

    if (!run_configuration_menu_state_machine()) {
        printf("Unable to get configuration information, will restart in 10 seconds...\n");
        sleep(10);
        esp_restart();
    }

    static httpd_handle_t server = NULL;

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, &server));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t sta_config = {
        .sta = {
            .bssid_set = false
        }
    };
    strlcpy((char *)sta_config.sta.ssid, s_wifi_ssid, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, s_wifi_password, sizeof(sta_config.sta.password));
    // printf("debug: WiFi credentials: %s %s\n", sta_config.sta.ssid, sta_config.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool run_configuration_menu_state_machine(void) {
    esp_err_t err = ESP_OK;
    configuration_state state = configuration_state_start;

    while (1) {
        if (state == configuration_state_start) {
            // Initialize NVS
            err = nvs_flash_init();
            if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
                printf("Failed to initialize NVS, erasing and retrying\n");
                // NVS partition was truncated and needs to be erased
                // Retry nvs_flash_init
                ESP_ERROR_CHECK(nvs_flash_erase());
                err = nvs_flash_init();
            }

            if (err != ESP_OK) {
                printf("Failed to initialize NVS\n");
                configuration_transition_to_state(&state, configuration_state_error);
                continue;
            }

            configuration_transition_to_state(&state, configuration_state_check_for_nvs_configuration_data);
        } else if (state == configuration_state_check_for_nvs_configuration_data) {
            nvs_handle my_handle = NULL;
            if (!open_nvs_handle(&my_handle)) {
                configuration_transition_to_state(&state, configuration_state_error);
                continue;
            }

            bool did_find_nvs_data = read_nvs_config_wifi_credentials(my_handle);
            bool did_find_calibration_data = read_nvs_config_calibration_data(my_handle);

            if (did_find_nvs_data && did_find_calibration_data) {
                configuration_transition_to_state(&state, configuration_state_querying_for_config_menu);
            } else {
                printf("Forcing configuration mode\n");
                configuration_transition_to_state(&state, configuration_state_starting_config_menu);
            }

            nvs_close(my_handle);            
        } else if (state == configuration_state_querying_for_config_menu) {
            bool should_enter_configuration_menu = false;
            for (int i = 0; i < CONFIGURATION_MENU_TIMEOUT_SECONDS && !should_enter_configuration_menu; i++) {
                printf("Press any key within the next %d seconds to enter configuration mode\n", CONFIGURATION_MENU_TIMEOUT_SECONDS - i);
                uint8_t ch = fgetc(stdin);
                if (ch != 255) {
                    printf("Key press detected\n");
                    should_enter_configuration_menu = true;
                    continue;
                }
                sleep(1);
            }
            if (should_enter_configuration_menu) {
                configuration_transition_to_state(&state, configuration_state_starting_config_menu);
            } else {
                configuration_transition_to_state(&state, configuration_state_success);
            }
        } else if (state == configuration_state_starting_config_menu) {
            /* Disable buffering on stdin */
            setvbuf(stdin, NULL, _IONBF, 0);

            printf("Entering configuration mode\n");
            sleep(1);

            /* Minicom, screen, idf_monitor send CR when ENTER key is pressed */
            esp_vfs_dev_uart_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
            /* Move the caret to the beginning of the next line on '\n' */
            esp_vfs_dev_uart_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

            /* Configure UART. Note that REF_TICK is used so that the baud rate remains
            * correct while APB frequency is changing in light sleep mode.
            */
            const uart_config_t uart_config = {
                    .baud_rate = 115200,
                    .data_bits = UART_DATA_8_BITS,
                    .parity = UART_PARITY_DISABLE,
                    .stop_bits = UART_STOP_BITS_1,
                    .use_ref_tick = true
            };
            ESP_ERROR_CHECK(uart_param_config(0, &uart_config));
            /* Install UART driver for interrupt-driven reads and writes */
            ESP_ERROR_CHECK(uart_driver_install(0, 256, 0, 0, NULL, 0));
            esp_vfs_dev_uart_use_driver(0);

            linenoiseSetDumbMode(1);
            configuration_transition_to_state(&state, configuration_state_running_config_menu);
        } else if (state == configuration_state_running_config_menu) {
            printf("\nChoose a setting to change:\n");
            printf("w - WiFi settings\n");
            printf("i - IP address settings\n");
            printf("c - ADC voltage measurement calibration\n");
            printf("r - Reboot\n");
            char *line = linenoise("> ");
            printf("\n");
            if (line == NULL) {
                continue;
            }
            char menu_choice_letter = line[0];
            linenoiseFree(line);

            switch (menu_choice_letter) {
                case 'w':
                    configuration_transition_to_state(&state, configuration_state_querying_for_wifi_credentials);
                    break;

                case 'c':
                    configuration_transition_to_state(&state, configuration_state_running_calibration_menu);
                    break;

                case 'r':
                    configuration_transition_to_state(&state, configuration_state_restarting);
                    break;
            }
        } else if (state == configuration_state_querying_for_wifi_credentials) {
            nvs_handle my_handle = NULL;
            if (!open_nvs_handle(&my_handle)) {
                configuration_transition_to_state(&state, configuration_state_error);
                continue;
            }

            bool did_update_wifi_credentials = false;
            char *line = linenoise("WiFi SSID: ");
            printf("\n");
            if (line) {
                strlcpy(s_wifi_ssid, line, WIFI_CREDENTIAL_BUFFER_SIZE);
                linenoiseFree(line);
                line = linenoise("WiFi Password: ");
                printf("\n");
                if (line) {
                    strlcpy(s_wifi_password, line, WIFI_CREDENTIAL_BUFFER_SIZE);
                    linenoiseFree(line);
                    printf("Updating WiFi credentials in NVS\n");
                    err = nvs_set_str(my_handle, NVS_KEY_WIFI_SSID, s_wifi_ssid);
                    if (err == ESP_OK) {
                        err = nvs_set_str(my_handle, NVS_KEY_WIFI_PASSWORD, s_wifi_password);
                    }
                    if (err == ESP_OK) {
                        did_update_wifi_credentials = true;
                    }
                }
            }
            if (did_update_wifi_credentials) {
                printf("Successfully updated WiFi credentials in NVS\n");
            } else {
                printf("Did not update WiFi credentials in NVS\n");
            }
            configuration_transition_to_state(&state, configuration_state_running_config_menu);
            nvs_close(my_handle);
        } else if (state == configuration_state_running_calibration_menu) {
            float raw_sample = averaged_adc_sample();
            float calibrated_sample = apply_calibration_to_adc_sample(raw_sample);
            printf("\nRaw ADC value: %.3f, calibrated value: %.3f, y-intercept: %.3f, slope: %.3f\n", raw_sample, calibrated_sample, s_adc_calibration_y_intercept, s_adc_calibration_slope);
            printf("Choose an option:\n");
            printf("i - change calibration y-intercept\n");
            printf("s - change calibration slope\n");
            printf("n - Commit current values to non-volatile storage\n");
            printf("c - cancel\n");
            char *line = linenoise("> ");
            printf("\n");
            if (line == NULL) {
                continue;
            }
            char menu_choice_letter = line[0];
            linenoiseFree(line);

            switch (menu_choice_letter) {
                case 'i':
                    configuration_transition_to_state(&state, configuration_state_querying_for_calibration_y_intercept);
                    break;

                case 's':
                    configuration_transition_to_state(&state, configuration_state_querying_for_calibration_slope);
                    break;

                case 'c':
                    configuration_transition_to_state(&state, configuration_state_running_config_menu);
                    break;

                case 'n':
                    if (write_nvs_config_calibration_data()) {
                        configuration_transition_to_state(&state, configuration_state_running_config_menu);
                    }
                    break;
            }
        } else if (state == configuration_state_querying_for_calibration_y_intercept) {
            query_float_value("Y-intercept: ", &s_adc_calibration_y_intercept);
            configuration_transition_to_state(&state, configuration_state_running_calibration_menu);
        } else if (state == configuration_state_querying_for_calibration_slope) {
            query_float_value("Slope: ", &s_adc_calibration_slope);
            configuration_transition_to_state(&state, configuration_state_running_calibration_menu);
        } else if (state == configuration_state_success) {
            printf("Configuration succeeded\n");
            return true;
        } else if (state == configuration_state_error) {
            printf("Configuration failed\n");
            configuration_transition_to_state(&state, configuration_state_restarting);
        } else if (state == configuration_state_restarting) {
            printf("Restarting...\n");
            sleep(1);
            esp_restart();
        } else {
            printf("Unknown configuration state %d\n", state);
            configuration_transition_to_state(&state, configuration_state_error);
        }
    }   
}

bool read_nvs_config_wifi_credentials(nvs_handle my_handle) {
    printf("Reading WiFi info from NVS ...\n");
    bool did_find_nvs_data = false;
    size_t buffer_length = WIFI_CREDENTIAL_BUFFER_SIZE;
    bzero(s_wifi_ssid, WIFI_CREDENTIAL_BUFFER_SIZE);
    bzero(s_wifi_password, WIFI_CREDENTIAL_BUFFER_SIZE);
    esp_err_t err = nvs_get_str(my_handle, NVS_KEY_WIFI_SSID, s_wifi_ssid, &buffer_length);
    if (err == ESP_OK) {
        buffer_length = WIFI_CREDENTIAL_BUFFER_SIZE;
        err = nvs_get_str(my_handle, NVS_KEY_WIFI_PASSWORD, s_wifi_password, &buffer_length);
        if (err == ESP_OK) {
            did_find_nvs_data = true;
        }
    }

    if (did_find_nvs_data) {
        printf("Found WiFi info in NVS, SSID = %s\n", s_wifi_ssid);
    } else {
        printf("Did not find WiFi info in NVS: %s\n", esp_err_to_name(err));
    }
    return did_find_nvs_data;
}

bool read_nvs_config_calibration_data(nvs_handle my_handle) {
    printf("Reading ADC calibration data from NVS ...\n");
    bool did_find_nvs_data = false;

    size_t buffer_length = sizeof(s_adc_calibration_y_intercept);
    esp_err_t err = nvs_get_blob(my_handle, NVS_KEY_ADC_CALIBRATION_Y_INTERCEPT, &s_adc_calibration_y_intercept, &buffer_length);
    if (err == ESP_OK) {
        buffer_length = sizeof(s_adc_calibration_slope);
        err = nvs_get_blob(my_handle, NVS_KEY_ADC_CALIBRATION_SLOPE, &s_adc_calibration_slope, &buffer_length);
        if (err == ESP_OK) {
            did_find_nvs_data = true;
        }
    }

    if (did_find_nvs_data) {
        printf("Found ADC calibration data in NVS, y-intercept = %.2f, slope = %.2f\n", s_adc_calibration_y_intercept, s_adc_calibration_slope);
    } else {
        printf("Did not find ADC calibration data in NVS: %s\n", esp_err_to_name(err));
    }
    return did_find_nvs_data;
}

bool write_nvs_config_calibration_data(void) {
    nvs_handle my_handle = NULL;
    if (!open_nvs_handle(&my_handle)) {        
        return false;
    }

    bool did_write = false;
    printf("Writing ADC calibration data to NVS ...\n");
    esp_err_t err = nvs_set_blob(my_handle, NVS_KEY_ADC_CALIBRATION_Y_INTERCEPT, &s_adc_calibration_y_intercept, sizeof(s_adc_calibration_y_intercept));
    if (err == ESP_OK) {
        err = nvs_set_blob(my_handle, NVS_KEY_ADC_CALIBRATION_SLOPE, &s_adc_calibration_slope, sizeof(s_adc_calibration_slope));
        if (err == ESP_OK) {
            did_write = true;
        }
    }

    if (!did_write) {
        printf("Unable to write ADC calibration data to NVS: %s\n", esp_err_to_name(err));
    }

    nvs_close(my_handle);

    return did_write;
}

bool query_float_value(char *prompt, float *out_value) {
    bool did_get_value = false;
    char *line = linenoise(prompt);
    printf("\n");
    if (line) {
        char *endptr;
        float value = strtof(line, &endptr);
        if (endptr != line) {
            *out_value = value;
            did_get_value = true;
        } else {
            printf("Unable to parse input as floating point: %s\n", line);
        }
        linenoiseFree(line);
    }
    return did_get_value;
}

bool open_nvs_handle(nvs_handle *handle) {
    printf("Opening Non-Volatile Storage (NVS) handle...\n");
    esp_err_t err = nvs_open("storage", NVS_READWRITE, handle);
    if (err != ESP_OK) {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

void configuration_transition_to_state(configuration_state *current_state, configuration_state new_state) {
    // printf("*** Configuration state transition from %s to %s\n", configuration_state_label_for_value(*current_state), configuration_state_label_for_value(new_state));
    *current_state = new_state;
}