/*
 * hal_microkernel_bridge.c
 * 
 * HAL component bridge to AILang microkernel
 * Provides zero-blocking interface between LinuxCNC HAL and microkernel shared memory
 * 
 * Compile: halcompile --install hal_microkernel_bridge.c
 * Load:    halcmd loadrt hal_microkernel_bridge
 * 
 * Creates pins:
 *   microkernel.pin.N.in   (float, IN)  - Write to microkernel pin N
 *   microkernel.pin.N.out  (float, OUT) - Read from microkernel pin N
 */

#include "rtapi.h"
#include "rtapi_app.h"
#include "hal.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/* Module info */
MODULE_AUTHOR("AILang + LinuxCNC");
MODULE_DESCRIPTION("Bridge between HAL and AILang microkernel");
MODULE_LICENSE("GPL");

/* Configuration */
#define MAX_PINS 16
#define SHARED_MEM_PATH "/tmp/hal_pins.shm"
#define SHARED_MEM_SIZE 4096

/* Shared memory layout matches microkernel:
 * [0-7]     Pin count
 * [8-15]    Update flag
 * [16-23]   Pin 0 value
 * [24-31]   Pin 1 value
 * ...
 */

/* HAL component data structure */
typedef struct {
    hal_float_t *pin_in[MAX_PINS];   /* Input pins (HAL -> microkernel) */
    hal_float_t *pin_out[MAX_PINS];  /* Output pins (microkernel -> HAL) */
    hal_bit_t *connected;             /* Status: connected to microkernel */
    hal_u32_t *update_count;          /* Total updates processed */
    hal_u32_t *error_count;           /* Errors encountered */
} hal_microkernel_t;

/* Global state */
static hal_microkernel_t *hal_data = NULL;
static int comp_id;
static volatile int64_t *shm_ptr = NULL;  /* Pointer to shared memory */
static int shm_fd = -1;

/* Forward declarations */
static void update_pins(void *arg, long period);
static int map_shared_memory(void);
static void unmap_shared_memory(void);

/*
 * Component initialization
 * Called once when component loads
 */
int rtapi_app_main(void) {
    int retval;
    int i;
    char pin_name[HAL_NAME_LEN + 1];
    
    /* Initialize component */
    comp_id = hal_init("microkernel");
    if (comp_id < 0) {
        rtapi_print_msg(RTAPI_MSG_ERR, 
            "MICROKERNEL: ERROR: hal_init() failed\n");
        return -1;
    }
    
    /* Allocate shared HAL memory */
    hal_data = hal_malloc(sizeof(hal_microkernel_t));
    if (hal_data == NULL) {
        rtapi_print_msg(RTAPI_MSG_ERR,
            "MICROKERNEL: ERROR: hal_malloc() failed\n");
        hal_exit(comp_id);
        return -1;
    }
    
    /* Initialize all pointers to NULL */
    memset(hal_data, 0, sizeof(hal_microkernel_t));
    
    /* Create HAL pins for each microkernel pin */
    for (i = 0; i < MAX_PINS; i++) {
        /* Input pin: HAL writes, we read and send to microkernel */
        snprintf(pin_name, sizeof(pin_name), "microkernel.pin.%d.in", i);
        retval = hal_pin_float_new(pin_name, HAL_IN, 
                                    &(hal_data->pin_in[i]), comp_id);
        if (retval != 0) {
            rtapi_print_msg(RTAPI_MSG_ERR,
                "MICROKERNEL: ERROR: failed to create pin %s\n", pin_name);
            hal_exit(comp_id);
            return retval;
        }
        
        /* Output pin: We read from microkernel and write to HAL */
        snprintf(pin_name, sizeof(pin_name), "microkernel.pin.%d.out", i);
        retval = hal_pin_float_new(pin_name, HAL_OUT,
                                    &(hal_data->pin_out[i]), comp_id);
        if (retval != 0) {
            rtapi_print_msg(RTAPI_MSG_ERR,
                "MICROKERNEL: ERROR: failed to create pin %s\n", pin_name);
            hal_exit(comp_id);
            return retval;
        }
    }
    
    /* Create status pins */
    retval = hal_pin_bit_new("microkernel.connected", HAL_OUT,
                             &(hal_data->connected), comp_id);
    if (retval != 0) {
        hal_exit(comp_id);
        return retval;
    }
    
    retval = hal_pin_u32_new("microkernel.update-count", HAL_OUT,
                             &(hal_data->update_count), comp_id);
    if (retval != 0) {
        hal_exit(comp_id);
        return retval;
    }
    
    retval = hal_pin_u32_new("microkernel.error-count", HAL_OUT,
                             &(hal_data->error_count), comp_id);
    if (retval != 0) {
        hal_exit(comp_id);
        return retval;
    }
    
    /* Map shared memory to microkernel */
    if (map_shared_memory() != 0) {
        rtapi_print_msg(RTAPI_MSG_WARN,
            "MICROKERNEL: WARNING: Failed to map shared memory\n");
        rtapi_print_msg(RTAPI_MSG_WARN,
            "MICROKERNEL: Component loaded but not connected to microkernel\n");
        *(hal_data->connected) = 0;
    } else {
        rtapi_print_msg(RTAPI_MSG_INFO,
            "MICROKERNEL: Connected to microkernel shared memory\n");
        *(hal_data->connected) = 1;
    }
    
    /* Export update function to realtime thread */
    retval = hal_export_funct("microkernel.update", update_pins,
                              hal_data, 0, 0, comp_id);
    if (retval != 0) {
        rtapi_print_msg(RTAPI_MSG_ERR,
            "MICROKERNEL: ERROR: failed to export function\n");
        unmap_shared_memory();
        hal_exit(comp_id);
        return retval;
    }
    
    /* Initialize counters */
    *(hal_data->update_count) = 0;
    *(hal_data->error_count) = 0;
    
    hal_ready(comp_id);
    
    rtapi_print_msg(RTAPI_MSG_INFO,
        "MICROKERNEL: Bridge component loaded with %d pin pairs\n", MAX_PINS);
    
    return 0;
}

/*
 * Map shared memory file
 * Called during initialization
 */
static int map_shared_memory(void) {
    /* Open shared memory file (created by microkernel) */
    shm_fd = open(SHARED_MEM_PATH, O_RDWR);
    if (shm_fd < 0) {
        rtapi_print_msg(RTAPI_MSG_ERR,
            "MICROKERNEL: Cannot open %s (is microkernel running?)\n",
            SHARED_MEM_PATH);
        return -1;
    }
    
    /* Map it into our address space */
    shm_ptr = (volatile int64_t *)mmap(NULL, SHARED_MEM_SIZE,
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED, shm_fd, 0);
    
    if (shm_ptr == MAP_FAILED) {
        rtapi_print_msg(RTAPI_MSG_ERR,
            "MICROKERNEL: mmap() failed\n");
        close(shm_fd);
        shm_fd = -1;
        shm_ptr = NULL;
        return -1;
    }
    
    rtapi_print_msg(RTAPI_MSG_INFO,
        "MICROKERNEL: Shared memory mapped at %p\n", shm_ptr);
    
    return 0;
}

/*
 * Unmap shared memory
 * Called during cleanup
 */
static void unmap_shared_memory(void) {
    if (shm_ptr != NULL) {
        munmap((void *)shm_ptr, SHARED_MEM_SIZE);
        shm_ptr = NULL;
    }
    
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
}

/*
 * Update function - called every servo period
 * THIS RUNS IN REALTIME THREAD - MUST BE FAST, NO BLOCKING!
 */
static void update_pins(void *arg, long period) {
    hal_microkernel_t *data = (hal_microkernel_t *)arg;
    int i;
    int pin_offset;
    hal_float_t value;
    
    /* If not connected to microkernel, do nothing */
    if (shm_ptr == NULL) {
        *(data->error_count) += 1;
        return;
    }
    
    /* 
     * Write HAL input pins to microkernel shared memory
     * These are pins that HAL is writing TO us, which we forward to microkernel
     */
    for (i = 0; i < MAX_PINS; i++) {
        /* Read from HAL pin (HAL -> us) */
        value = *(data->pin_in[i]);
        
        /* Write to microkernel shared memory (us -> microkernel) */
        /* Offset: 16 bytes header + (pin_id * 8 bytes per pin) */
        pin_offset = 2 + i;  /* shm_ptr is int64_t*, so index directly */
        
        /* Convert float to int64_t for shared memory */
        /* This preserves the bit pattern for floats */
        union {
            hal_float_t f;
            int64_t i;
        } convert;
        convert.f = value;
        
        shm_ptr[pin_offset] = convert.i;
    }
    
    /*
     * Read microkernel shared memory and write to HAL output pins
     * These are pins that microkernel writes, which we read and give to HAL
     */
    for (i = 0; i < MAX_PINS; i++) {
        pin_offset = 2 + i;
        
        /* Read from microkernel shared memory */
        union {
            hal_float_t f;
            int64_t i;
        } convert;
        convert.i = shm_ptr[pin_offset];
        
        /* Write to HAL output pin */
        *(data->pin_out[i]) = convert.f;
    }
    
    /* Set update flag in shared memory to notify microkernel */
    shm_ptr[1] = 1;  /* Offset 8 bytes = index 1 */
    
    /* Increment update counter */
    *(data->update_count) += 1;
}

/*
 * Component cleanup
 * Called when component unloads
 */
void rtapi_app_exit(void) {
    rtapi_print_msg(RTAPI_MSG_INFO,
        "MICROKERNEL: Shutting down bridge component\n");
    
    unmap_shared_memory();
    hal_exit(comp_id);
}
