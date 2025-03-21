#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"

static const char *TAG = "TWDT_Example";

// TWDT configuration parameters
#define WATCHDOG_TIMEOUT_MS         5000    // 5 seconds timeout

// GPIO for LED indicators
#define STATUS_LED                  GPIO_NUM_2
#define STATUS_LED_2                 GPIO_NUM_15

// Event group bits
#define RECOVERY_ACTIVE_BIT         BIT0

// Global variables
static EventGroupHandle_t event_group;
static esp_task_wdt_user_handle_t twdt_user_handle;
static esp_task_wdt_user_handle_t twdt_user_2_handle;
static volatile bool g_watchdog_timeout_occurred = false;

// Define a buffer to store the task/user names
#define MAX_TASK_NAME_LEN 32
static char failed_task_name[MAX_TASK_NAME_LEN];
static bool capturing_task_name = false;
static bool task_name_captured = false;

// Forward declarations
static void init_gpio(void);
static void test_task(void *pvParameters);
static void recovery_task(void *pvParameters);
static void init_watchdog(void);

//---------------------------------------------------------------------
// Custom TWDT User Handler - MUST be minimal and ISR-safe
//---------------------------------------------------------------------
void esp_task_wdt_isr_user_handler(void)
{
    // Just set a flag - DO NOT use ESP_LOG functions here
    g_watchdog_timeout_occurred = true;
    
    // Set recovery bit in event group (from ISR context)
    if (event_group != NULL) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xEventGroupSetBitsFromISR(event_group, RECOVERY_ACTIVE_BIT, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

//--------------------------------------------------------------------
// Custom Message Handler to capture task names
//--------------------------------------------------------------------
static void twdt_msg_handler(void *opaque, const char *msg) {
    // Check if we're starting a new task entry
    if (strstr(msg, " -") != NULL) {
        capturing_task_name = true;
        failed_task_name[0] = '\0'; // Reset the buffer
        return;
    }
    
    // If we're in task name capturing mode and this isn't a CPU info message
    if (capturing_task_name && strstr(msg, "(CPU") == NULL) {
        // This should be the task name
        if (strlen(msg) < MAX_TASK_NAME_LEN) {
            strncpy(failed_task_name, msg, MAX_TASK_NAME_LEN - 1);
            failed_task_name[MAX_TASK_NAME_LEN - 1] = '\0'; // Ensure null termination
            task_name_captured = true;
        }
    }
    
    // If we see a CPU info message, we're done capturing the task name
    if (strstr(msg, "(CPU") != NULL) {
        capturing_task_name = false;
    }
}

//---------------------------------------------------------------------
// Initialize GPIO for status LED
//---------------------------------------------------------------------
static void init_gpio(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << STATUS_LED | 1ULL << STATUS_LED_2);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    // Initialize LED to off
    gpio_set_level(STATUS_LED, 0);
    gpio_set_level(STATUS_LED_2, 0);
}

//---------------------------------------------------------------------
// Initialize Task Watchdog Timer
//---------------------------------------------------------------------
static void init_watchdog(void)
{
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WATCHDOG_TIMEOUT_MS,
        .idle_core_mask = 0,          // No idle core monitoring
        .trigger_panic = false,       // Don't trigger panic so our custom handler executes
    };
    
    ESP_ERROR_CHECK(esp_task_wdt_init(&twdt_config));
    ESP_LOGI(TAG, "TWDT initialized with timeout: %d ms", WATCHDOG_TIMEOUT_MS);
}

//---------------------------------------------------------------------
// Test Task - Will trigger the watchdog
//---------------------------------------------------------------------
static void test_task(void *pvParameters)
{
    // Register this task with TWDT
    ESP_ERROR_CHECK(esp_task_wdt_add_user("test_user", &twdt_user_handle));
    ESP_LOGI(TAG, "Test task registered with TWDT");
    
    int counter = 0;
    
    while (1) {
        counter++;
        ESP_LOGI(TAG, "Test task running, counter = %d", counter);
        
        // Reset watchdog for the first 3 iterations
        if (counter <= 3) {
            ESP_LOGI(TAG, "Resetting watchdog timer (%d/3)", counter);
            ESP_ERROR_CHECK(esp_task_wdt_reset_user(twdt_user_handle));
        } else if (counter == 4) {
            // On the 4th iteration, don't reset and warn about it
            ESP_LOGW(TAG, "Not resetting watchdog - will trigger timeout in %d ms", WATCHDOG_TIMEOUT_MS);
        } else if (counter > 10 && counter < 20) {
            // After recovery, start resetting again
            ESP_LOGI(TAG, "Resuming normal operation, resetting watchdog");
            ESP_ERROR_CHECK(esp_task_wdt_reset_user(twdt_user_handle));
        } else if (counter > 20 && counter < 30) {
            // After recovery not reset again for testing
            ESP_LOGI(TAG, "Not resetting watchdog - will trigger timeout in %d ms", WATCHDOG_TIMEOUT_MS);
        } else if (counter > 30 && counter < 40) {
            // After recovery, start resetting again
            counter = 0;
            ESP_LOGI(TAG, "Resuming normal operation, resetting watchdog");
            ESP_ERROR_CHECK(esp_task_wdt_reset_user(twdt_user_handle));
        }
        
        // Blink LED to show task is running
        gpio_set_level(STATUS_LED, counter % 2);
        
        // Delay for 1 second
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

//---------------------------------------------------------------------
// Test Task - Will trigger the watchdog
//---------------------------------------------------------------------
static void test_2_task(void *pvParameters)
{
    // Register this task with TWDT
    ESP_ERROR_CHECK(esp_task_wdt_add_user("test_2_user", &twdt_user_2_handle));
    ESP_LOGI(TAG, "Test task registered with TWDT");
    
    int counter = 0;
    
    while (1) {
        counter++;
        ESP_LOGI(TAG, "Test task running, counter_2 = %d", counter);
        
        // Reset watchdog for the first 3 iterations
        if (counter <= 3) {
            ESP_LOGI(TAG, "Resetting watchdog timer (%d/3)", counter);
            ESP_ERROR_CHECK(esp_task_wdt_reset_user(twdt_user_2_handle));
        } else if (counter == 4) {
            // On the 4th iteration, don't reset and warn about it
            ESP_LOGW(TAG, "Not resetting watchdog - will trigger timeout in %d ms", WATCHDOG_TIMEOUT_MS);
        } else if (counter > 10 && counter < 20) {
            // After recovery, start resetting again
            ESP_LOGI(TAG, "Resuming normal operation, resetting watchdog");
            ESP_ERROR_CHECK(esp_task_wdt_reset_user(twdt_user_2_handle));
        } else if (counter > 20 && counter < 30) {
            // After recovery not reset again for testing
            ESP_LOGI(TAG, "Not resetting watchdog - will trigger timeout in %d ms", WATCHDOG_TIMEOUT_MS);
        } else if (counter > 30 && counter < 40) {
            // After recovery, start resetting again
            counter = 0;
            ESP_LOGI(TAG, "Resuming normal operation, resetting watchdog");
            ESP_ERROR_CHECK(esp_task_wdt_reset_user(twdt_user_2_handle));
        }
        
        // Blink LED to show task is running
        gpio_set_level(STATUS_LED_2, counter % 2);
        
        // Delay for 1 second
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

//---------------------------------------------------------------------
// Recovery Task - Handles watchdog timeout recovery
//---------------------------------------------------------------------
static void recovery_task(void *pvParameters)
{
    while (1) {
        // Wait for recovery bit to be set
        EventBits_t bits = xEventGroupWaitBits(
            event_group,
            RECOVERY_ACTIVE_BIT,
            pdTRUE,  // Clear on exit
            pdFALSE, // Don't wait for all bits
            portMAX_DELAY);
            
        ESP_LOGI(TAG, "----------------A---------------------");
        // Reset the capture flag
        capturing_task_name = false;
        task_name_captured = false;
        failed_task_name[0] = '\0';

        // Get more detailed information about which tasks/users triggered the watchdog
        int failing_cpus = 0;
        esp_err_t err = esp_task_wdt_print_triggered_tasks(twdt_msg_handler, NULL, &failing_cpus);
        
        // if (task_name_captured) {
        //     ESP_LOGI(TAG, "Failed task/user name: %s", failed_task_name);
            
        //     // Now you can take specific actions based on which task failed
        //     if (strcmp(failed_task_name, "test_2_user") == 0) {
        //         ESP_LOGI(TAG, "test_2_user failed, taking specific recovery action...");
        //         // Recovery action specific to test_2_user
        //     }
        //     // Add more conditions for other tasks as needed
        // }

        ESP_LOGI(TAG, "----------------B---------------------");

        if (bits & RECOVERY_ACTIVE_BIT) {
            // Check our global flag
            if (g_watchdog_timeout_occurred) {
                // Reset the flag
                g_watchdog_timeout_occurred = false;

                // Now it's safe to log
                ESP_LOGE(TAG, "Custom TWDT handler was invoked! Task failed to reset the watchdog in time.");
                ESP_LOGE(TAG, "Performing recovery actions...");

                if (task_name_captured) {
                    if (strcmp(failed_task_name, "test_user") == 0) {
                        ESP_LOGI(TAG, "test_user failed, taking specific recovery action...");
                        // Recovery action specific to test_2_user
                        
                        // Perform recovery actions - blink LED rapidly to indicate recovery
                        for (int i = 0; i < 10; i++) {
                            gpio_set_level(STATUS_LED, 1);
                            vTaskDelay(pdMS_TO_TICKS(100));
                            gpio_set_level(STATUS_LED, 0);
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                    }
                    if (strcmp(failed_task_name, "test_2_user") == 0) {
                        ESP_LOGI(TAG, "test_2_user failed, taking specific recovery action...");
                        // Recovery action specific to test_2_user
                        
                        // Perform recovery actions - blink LED rapidly to indicate recovery
                        for (int i = 0; i < 10; i++) {
                            gpio_set_level(STATUS_LED_2, 1);
                            vTaskDelay(pdMS_TO_TICKS(100));
                            gpio_set_level(STATUS_LED_2, 0);
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                    }
                }
                
                ESP_LOGI(TAG, "Recovery complete");
            }
        }
        
        // Short delay before checking again
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

//---------------------------------------------------------------------
// Main Application Entry Point
//---------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "Starting Task Watchdog Example");
    
    // Initialize GPIO for status LED
    init_gpio();
    
    // Create event group
    event_group = xEventGroupCreate();
    
    // Initialize the Task Watchdog Timer
    init_watchdog();
    
    // Create the recovery task
    xTaskCreate(recovery_task, "recovery_task", 4096, NULL, 5, NULL);
    
    // Create the test task that will trigger the watchdog
    xTaskCreate(test_task, "test_task", 2048, NULL, 4, NULL);

    xTaskCreate(test_2_task, "test_2_task", 2048, NULL, 4, NULL);
    
    ESP_LOGI(TAG, "All tasks created, system running");
}
