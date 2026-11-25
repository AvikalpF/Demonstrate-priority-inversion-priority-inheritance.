#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_timer.h"

/*
 * Software for Embedded Systems Lab
 *
 * Experiment 1:
 * Priority Inversion and Priority Inheritance
 *
 * Experiment 2:
 * Shared Memory Access Using a Mutex
 *
 * Target: ESP32-S3
 * Framework: ESP-IDF 6.1
 * RTOS: FreeRTOS
 *
 * All experiment tasks are pinned to CPU1.
 */


/* ============================================================
 * EXPERIMENT 1: PRIORITY INVERSION
 * ============================================================
 */

#define LOW_PRIORITY          1
#define MEDIUM_PRIORITY       2
#define HIGH_PRIORITY         3

#define EXPERIMENT_CORE       1

#define LOW_WORK_MS           1500
#define MEDIUM_WORK_MS        2500
#define HIGH_START_DELAY_MS   100

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
 * Acquires the shared resource and performs CPU work while
 * holding it.
 */
static void low_task(void *arg)
{
    printf("[LOW] Requesting shared resource.\n");

    xSemaphoreTake(resource_lock, portMAX_DELAY);

    printf("[LOW] Acquired resource; entering critical section.\n");

    /*
     * Notify app_main() that LOW owns the resource.
     */
    xSemaphoreGive(low_locked);

    /*
     * Keep the resource while performing CPU-intensive work.
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
 * Case A:
 * MEDIUM can preempt LOW while HIGH waits for the resource.
 *
 * Case B:
 * Priority inheritance can temporarily raise LOW's priority,
 * allowing LOW to finish its critical section.
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
 * Requests the same resource currently held by LOW.
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
 * Runs one priority inversion experiment.
 *
 * mutex_mode = 0:
 * Binary semaphore without priority inheritance.
 *
 * mutex_mode = 1:
 * Mutex with priority inheritance.
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

        if (low_locked != NULL) {
            vSemaphoreDelete(low_locked);
        }

        if (start_medium != NULL) {
            vSemaphoreDelete(start_medium);
        }

        if (start_high != NULL) {
            vSemaphoreDelete(start_high);
        }

        if (low_done != NULL) {
            vSemaphoreDelete(low_done);
        }

        if (medium_done != NULL) {
            vSemaphoreDelete(medium_done);
        }

        if (high_done != NULL) {
            vSemaphoreDelete(high_done);
        }

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
     * Allow MEDIUM to begin before HIGH requests the resource.
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
     * Wait for all three tasks to finish before deleting
     * their synchronization objects.
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


/* ============================================================
 * EXPERIMENT 2: SHARED MEMORY WITH A MUTEX
 * ============================================================
 */

/*
 * Shared data structure.
 *
 * Both tasks access this structure.
 * Every read-modify-write operation on its fields is protected
 * by shared_memory_mutex.
 */
typedef struct {
    uint32_t counter;
    uint32_t task_a_updates;
    uint32_t task_b_updates;
} shared_data_t;


/*
 * Shared global data.
 *
 * In this experiment, both FreeRTOS tasks access the same
 * RAM-resident structure.
 */
static shared_data_t shared_data = {0};


/*
 * Mutex protecting shared_data.
 */
static SemaphoreHandle_t shared_memory_mutex;


/*
 * Counting semaphore used to notify app_main() that the
 * two worker tasks have completed.
 *
 * A counting semaphore is used because both tasks must be
 * able to signal completion without losing a notification.
 */
static SemaphoreHandle_t shared_tasks_done;


/*
 * Number of updates performed by each task.
 */
#define SHARED_TASK_ITERATIONS    10000

/*
 * Task priorities for Experiment 2.
 */
#define SHARED_TASK_PRIORITY      2


/*
 * TASK A
 *
 * Increments the shared counter and its own update count.
 *
 * The mutex protects the complete read-modify-write operation
 * on the shared data structure.
 */
static void shared_task_a(void *arg)
{
    printf("[TASK A] Started.\n");

    for (uint32_t i = 0; i < SHARED_TASK_ITERATIONS; i++) {

        if (xSemaphoreTake(
                shared_memory_mutex,
                portMAX_DELAY) == pdTRUE) {

            shared_data.counter++;

            shared_data.task_a_updates++;

            xSemaphoreGive(shared_memory_mutex);
        }

        /*
         * Periodically yield so TASK B can also execute.
         * The delay is outside the critical section.
         */
        if ((i % 100) == 0) {
            vTaskDelay(1);
        }
    }

    printf("[TASK A] Completed %" PRIu32 " updates.\n",
           (uint32_t)SHARED_TASK_ITERATIONS);

    xSemaphoreGive(shared_tasks_done);

    vTaskDelete(NULL);
}


/*
 * TASK B
 *
 * Accesses the same shared data structure as TASK A.
 *
 * The mutex ensures that TASK A and TASK B cannot modify
 * the protected fields simultaneously.
 */
static void shared_task_b(void *arg)
{
    printf("[TASK B] Started.\n");

    for (uint32_t i = 0; i < SHARED_TASK_ITERATIONS; i++) {

        if (xSemaphoreTake(
                shared_memory_mutex,
                portMAX_DELAY) == pdTRUE) {

            shared_data.counter++;

            shared_data.task_b_updates++;

            xSemaphoreGive(shared_memory_mutex);
        }

        /*
         * Periodically yield so TASK A can also execute.
         * The delay is outside the critical section.
         */
        if ((i % 100) == 0) {
            vTaskDelay(1);
        }
    }

    printf("[TASK B] Completed %" PRIu32 " updates.\n",
           (uint32_t)SHARED_TASK_ITERATIONS);

    xSemaphoreGive(shared_tasks_done);

    vTaskDelete(NULL);
}


/*
 * Runs Experiment 2.
 *
 * Demonstrates:
 *
 * 1. Shared global memory accessed by multiple tasks.
 * 2. Mutual exclusion using a FreeRTOS mutex.
 * 3. Protection against lost updates.
 * 4. Verification of the final shared counter.
 */
static void run_shared_memory_experiment(void)
{
    printf("\n\n");
    printf("============================================\n");
    printf("EXPERIMENT 2: SHARED MEMORY WITH MUTEX\n");
    printf("============================================\n");

    /*
     * Initialize the shared data before creating the tasks.
     */
    shared_data.counter = 0;
    shared_data.task_a_updates = 0;
    shared_data.task_b_updates = 0;

    /*
     * Create the mutex.
     */
    shared_memory_mutex = xSemaphoreCreateMutex();

    if (shared_memory_mutex == NULL) {
        printf("[ERROR] Failed to create shared memory mutex.\n");
        return;
    }

    /*
     * Create a counting semaphore with capacity two.
     *
     * Each worker task gives one notification after completing.
     */
    shared_tasks_done = xSemaphoreCreateCounting(2, 0);

    if (shared_tasks_done == NULL) {
        printf("[ERROR] Failed to create completion semaphore.\n");

        vSemaphoreDelete(shared_memory_mutex);
        return;
    }

    printf("Shared counter initial value: 0\n");
    printf("Updates per task: %" PRIu32 "\n",
           (uint32_t)SHARED_TASK_ITERATIONS);

    printf("Creating TASK A and TASK B...\n\n");

    /*
     * Create TASK A.
     */
    BaseType_t task_a_result = xTaskCreatePinnedToCore(
        shared_task_a,
        "shared_task_a",
        3072,
        NULL,
        SHARED_TASK_PRIORITY,
        NULL,
        EXPERIMENT_CORE
    );

    if (task_a_result != pdPASS) {
        printf("[ERROR] Failed to create TASK A.\n");

        vSemaphoreDelete(shared_tasks_done);
        vSemaphoreDelete(shared_memory_mutex);
        return;
    }

    /*
     * Create TASK B.
     */
    BaseType_t task_b_result = xTaskCreatePinnedToCore(
        shared_task_b,
        "shared_task_b",
        3072,
        NULL,
        SHARED_TASK_PRIORITY,
        NULL,
        EXPERIMENT_CORE
    );

    if (task_b_result != pdPASS) {
        printf("[ERROR] Failed to create TASK B.\n");

        /*
         * TASK A is already running, so wait for it to finish
         * before deleting the objects it uses.
         */
        xSemaphoreTake(shared_tasks_done, portMAX_DELAY);

        vSemaphoreDelete(shared_tasks_done);
        vSemaphoreDelete(shared_memory_mutex);
        return;
    }

    /*
     * Wait for both tasks to complete.
     */
    xSemaphoreTake(shared_tasks_done, portMAX_DELAY);
    xSemaphoreTake(shared_tasks_done, portMAX_DELAY);

    /*
     * Both tasks have completed.
     *
     * Read the final values while holding the mutex.
     */
    uint32_t final_counter;
    uint32_t task_a_total;
    uint32_t task_b_total;

    if (xSemaphoreTake(
            shared_memory_mutex,
            portMAX_DELAY) == pdTRUE) {

        final_counter = shared_data.counter;
        task_a_total = shared_data.task_a_updates;
        task_b_total = shared_data.task_b_updates;

        xSemaphoreGive(shared_memory_mutex);
    } else {
        printf("[ERROR] Failed to read shared data.\n");

        vSemaphoreDelete(shared_tasks_done);
        vSemaphoreDelete(shared_memory_mutex);
        return;
    }

    /*
     * Display the results.
     */
    printf("\n");
    printf("--------------------------------------------\n");
    printf("SHARED MEMORY RESULTS\n");
    printf("--------------------------------------------\n");

    printf("Expected counter       : %" PRIu32 "\n",
           (uint32_t)(2 * SHARED_TASK_ITERATIONS));

    printf("Actual counter         : %" PRIu32 "\n",
           final_counter);

    printf("TASK A updates         : %" PRIu32 "\n",
           task_a_total);

    printf("TASK B updates         : %" PRIu32 "\n",
           task_b_total);

    printf("--------------------------------------------\n");

    /*
     * Verify the final counter and individual task totals.
     */
    if (final_counter == (2 * SHARED_TASK_ITERATIONS) &&
        task_a_total == SHARED_TASK_ITERATIONS &&
        task_b_total == SHARED_TASK_ITERATIONS) {

        printf("[PASS] Shared memory synchronization verified.\n");
        printf("[PASS] All updates were accounted for.\n");

    } else {

        printf("[FAIL] Shared memory result mismatch.\n");
    }

    printf("\n[RESULT] Experiment 2 completed.\n");

    /*
     * All worker tasks have finished, so the synchronization
     * objects can now be deleted.
     */
    vSemaphoreDelete(shared_tasks_done);
    vSemaphoreDelete(shared_memory_mutex);
}


/* ============================================================
 * APPLICATION ENTRY POINT
 * ============================================================
 */

void app_main(void)
{
    printf("\n");
    printf("============================================\n");
    printf(" SOFTWARE FOR EMBEDDED SYSTEMS LAB\n");
    printf(" ESP32-S3 | ESP-IDF 6.1 | FreeRTOS\n");
    printf("============================================\n");

    /*
     * EXPERIMENT 1
     *
     * Case A: Binary semaphore without priority inheritance.
     */
    printf("\n");
    printf("EXPERIMENT 1: PRIORITY INVERSION\n");

    printf("\nTask priorities:\n");
    printf("LOW    = %d\n", LOW_PRIORITY);
    printf("MEDIUM = %d\n", MEDIUM_PRIORITY);
    printf("HIGH   = %d\n", HIGH_PRIORITY);

    run_experiment(0);

    /*
     * Case B: Mutex with priority inheritance.
     */
    run_experiment(1);

    printf("\nAll priority inversion cases completed.\n");

    /*
     * EXPERIMENT 2
     *
     * Shared global memory protected by a mutex.
     */
    run_shared_memory_experiment();

    printf("\n");
    printf("============================================\n");
    printf("ALL EXPERIMENTS COMPLETED\n");
    printf("============================================\n");
}