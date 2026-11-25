#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_timer.h"

/*
 * Software for Embedded Systems Lab
 * Experiment 1: Priority Inversion and Priority Inheritance
 *
 * Target: ESP32-S3
 * Framework: ESP-IDF 6.1
 * RTOS: FreeRTOS
 *
 * All experiment tasks are pinned to CPU1.
 *
 * Case A: Binary semaphore -- no priority inheritance.
 * Case B: Mutex -- priority inheritance enabled.
 */

#define LOW_PRIORITY       1
#define MEDIUM_PRIORITY    2
#define HIGH_PRIORITY      3

#define EXPERIMENT_CORE    1

#define LOW_WORK_MS        1500
#define MEDIUM_WORK_MS     2500
#define HIGH_START_DELAY_MS 100

static SemaphoreHandle_t resource_lock;
static SemaphoreHandle_t low_locked;
static SemaphoreHandle_t start_medium;
static SemaphoreHandle_t start_high;

static SemaphoreHandle_t low_done;
static SemaphoreHandle_t medium_done;
static SemaphoreHandle_t high_done;

static int use_mutex;

/*
 * Performs CPU work for a specified duration.
 *
 * This is intentionally a busy loop. It does not block or yield,
 * allowing task-priority effects to become observable.
 */
static void busy_work_ms(uint32_t duration_ms)
{
    int64_t start_time = esp_timer_get_time();
    int64_t duration_us = (int64_t)duration_ms * 1000;

    while ((esp_timer_get_time() - start_time) < duration_us) {
        /* Intentionally busy: do not add vTaskDelay() here. */
    }
}

/*
 * LOW-PRIORITY TASK
 *
 * Acquires the shared resource and performs CPU work while holding it.
 */
static void low_task(void *arg)
{
    printf("[LOW] Requesting shared resource.\n");

    xSemaphoreTake(resource_lock, portMAX_DELAY);

    printf("[LOW] Acquired resource; entering critical section.\n");

    /*
     * Tell app_main() that the resource is held.
     * app_main() can now start the medium-priority task.
     */
    xSemaphoreGive(low_locked);

    /*
     * Keep the resource while doing CPU-intensive work.
     */
    busy_work_ms(LOW_WORK_MS);

    printf("[LOW] Releasing shared resource.\n");

    xSemaphoreGive(resource_lock);

    xSemaphoreGive(low_done);

    vTaskDelete(NULL);
}

/*
 * MEDIUM-PRIORITY TASK
 *
 * Does not use the shared resource.
 *
 * In Case A, this task can preempt LOW while HIGH waits.
 * In Case B, priority inheritance can temporarily raise LOW
 * above MEDIUM, allowing LOW to finish its critical section.
 */
static void medium_task(void *arg)
{
    xSemaphoreTake(start_medium, portMAX_DELAY);

    printf("[MEDIUM] Started CPU-intensive work.\n");

    busy_work_ms(MEDIUM_WORK_MS);

    printf("[MEDIUM] Finished CPU-intensive work.\n");

    xSemaphoreGive(medium_done);

    vTaskDelete(NULL);
}

/*
 * HIGH-PRIORITY TASK
 *
 * Requests the same resource as LOW.
 */
static void high_task(void *arg)
{
    xSemaphoreTake(start_high, portMAX_DELAY);

    int64_t request_time = esp_timer_get_time();

    printf("[HIGH] Requesting shared resource.\n");

    xSemaphoreTake(resource_lock, portMAX_DELAY);

    int64_t acquired_time = esp_timer_get_time();

    printf("[HIGH] Acquired resource after %" PRId64 " ms.\n",
           (acquired_time - request_time) / 1000);

    xSemaphoreGive(resource_lock);

    printf("[HIGH] Released resource.\n");

    xSemaphoreGive(high_done);

    vTaskDelete(NULL);
}

/*
 * Runs one experiment.
 *
 * mutex_mode = 0: binary semaphore; no priority inheritance.
 * mutex_mode = 1: mutex; priority inheritance enabled.
 */
static void run_experiment(int mutex_mode)
{
    use_mutex = mutex_mode;

    printf("\n");
    printf("============================================\n");

    if (use_mutex) {
        printf("CASE B: MUTEX WITH PRIORITY INHERITANCE\n");
        resource_lock = xSemaphoreCreateMutex();
    } else {
        printf("CASE A: BINARY SEMAPHORE WITHOUT PI\n");
        resource_lock = xSemaphoreCreateBinary();

        /*
         * Binary semaphores are initially empty.
         * Give one token to represent an available resource.
         */
        if (resource_lock != NULL) {
            xSemaphoreGive(resource_lock);
        }
    }

    printf("============================================\n");

    if (resource_lock == NULL) {
        printf("[ERROR] Failed to create resource lock.\n");
        return;
    }

    low_locked   = xSemaphoreCreateBinary();
    start_medium = xSemaphoreCreateBinary();
    start_high   = xSemaphoreCreateBinary();

    low_done     = xSemaphoreCreateBinary();
    medium_done  = xSemaphoreCreateBinary();
    high_done    = xSemaphoreCreateBinary();

    if (low_locked == NULL ||
        start_medium == NULL ||
        start_high == NULL ||
        low_done == NULL ||
        medium_done == NULL ||
        high_done == NULL) {

        printf("[ERROR] Failed to create synchronization objects.\n");

        /*
         * In this error path, free successfully created objects.
         */
        if (low_locked != NULL)   vSemaphoreDelete(low_locked);
        if (start_medium != NULL) vSemaphoreDelete(start_medium);
        if (start_high != NULL)   vSemaphoreDelete(start_high);
        if (low_done != NULL)     vSemaphoreDelete(low_done);
        if (medium_done != NULL)  vSemaphoreDelete(medium_done);
        if (high_done != NULL)    vSemaphoreDelete(high_done);

        vSemaphoreDelete(resource_lock);
        return;
    }

    /*
     * Start LOW first.
     */
    BaseType_t result = xTaskCreatePinnedToCore(
        low_task,
        "low_task",
        3072,
        NULL,
        LOW_PRIORITY,
        NULL,
        EXPERIMENT_CORE
    );

    if (result != pdPASS) {
        printf("[ERROR] Failed to create LOW task.\n");
        goto cleanup;
    }

    /*
     * Wait until LOW owns the resource.
     */
    xSemaphoreTake(low_locked, portMAX_DELAY);

    /*
     * Start MEDIUM while LOW holds the resource.
     */
    result = xTaskCreatePinnedToCore(
        medium_task,
        "medium_task",
        3072,
        NULL,
        MEDIUM_PRIORITY,
        NULL,
        EXPERIMENT_CORE
    );

    if (result != pdPASS) {
        printf("[ERROR] Failed to create MEDIUM task.\n");
        goto cleanup;
    }

    xSemaphoreGive(start_medium);

    /*
     * Let MEDIUM begin running before HIGH requests the resource.
     */
    vTaskDelay(pdMS_TO_TICKS(HIGH_START_DELAY_MS));

    result = xTaskCreatePinnedToCore(
        high_task,
        "high_task",
        3072,
        NULL,
        HIGH_PRIORITY,
        NULL,
        EXPERIMENT_CORE
    );

    if (result != pdPASS) {
        printf("[ERROR] Failed to create HIGH task.\n");
        goto cleanup;
    }

    xSemaphoreGive(start_high);

    /*
     * Wait until every successfully created task has completed.
     *
     * The binary completion semaphores also ensure that we do not
     * destroy shared synchronization objects while tasks use them.
     */
    xSemaphoreTake(low_done, portMAX_DELAY);
    xSemaphoreTake(medium_done, portMAX_DELAY);
    xSemaphoreTake(high_done, portMAX_DELAY);

    printf("\n[RESULT] Experiment completed.\n");

cleanup:
    vSemaphoreDelete(low_locked);
    vSemaphoreDelete(start_medium);
    vSemaphoreDelete(start_high);

    vSemaphoreDelete(low_done);
    vSemaphoreDelete(medium_done);
    vSemaphoreDelete(high_done);

    vSemaphoreDelete(resource_lock);

    /*
     * Leave time between experiments.
     */
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void app_main(void)
{
    printf("\n");
    printf("============================================\n");
    printf(" SOFTWARE FOR EMBEDDED SYSTEMS LAB\n");
    printf(" ESP32-S3 | ESP-IDF 6.1 | FreeRTOS\n");
    printf(" Experiment 1: Priority Inversion\n");
    printf("============================================\n");

    printf("\nTask priorities:\n");
    printf("LOW    = %d\n", LOW_PRIORITY);
    printf("MEDIUM = %d\n", MEDIUM_PRIORITY);
    printf("HIGH   = %d\n", HIGH_PRIORITY);

    /*
     * Case A: Binary semaphore without priority inheritance.
     */
    run_experiment(0);

    /*
     * Case B: Mutex with priority inheritance.
     */
    run_experiment(1);

    printf("\nAll priority experiments completed.\n");
}
