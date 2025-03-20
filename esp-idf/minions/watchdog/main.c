#include <stdio.h>
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

// Event group bits
#define RECOVERY_ACTIVE_BIT         BIT0

// Global variables
static EventGroupHandle_t event_group;
static esp_task_wdt_user_handle_t twdt_user_handle;
static volatile bool g_watchdog_timeout_occurred = false;

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

//---------------------------------------------------------------------
// Initialize GPIO for status LED
//---------------------------------------------------------------------
static void init_gpio(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << STATUS_LED);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    // Initialize LED to off
    gpio_set_level(STATUS_LED, 0);
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
            
        if (bits & RECOVERY_ACTIVE_BIT) {
            // Check our global flag
            if (g_watchdog_timeout_occurred) {
                // Reset the flag
                g_watchdog_timeout_occurred = false;
                
                // Now it's safe to log
                ESP_LOGE(TAG, "Custom TWDT handler was invoked! Task failed to reset the watchdog in time.");
                ESP_LOGE(TAG, "Performing recovery actions...");
                
                // Perform recovery actions - blink LED rapidly to indicate recovery
                for (int i = 0; i < 10; i++) {
                    gpio_set_level(STATUS_LED, 1);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    gpio_set_level(STATUS_LED, 0);
                    vTaskDelay(pdMS_TO_TICKS(100));
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
    xTaskCreate(recovery_task, "recovery_task", 2048, NULL, 5, NULL);
    
    // Create the test task that will trigger the watchdog
    xTaskCreate(test_task, "test_task", 2048, NULL, 4, NULL);
    
    ESP_LOGI(TAG, "All tasks created, system running");
}
