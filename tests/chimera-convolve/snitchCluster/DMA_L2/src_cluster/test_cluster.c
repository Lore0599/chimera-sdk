// SPDX-FileCopyrightText: 2024 ETH Zurich and University of Bologna
// SPDX-License-Identifier: Apache-2.0

// Include Standard Libraries
#include <stdio.h>
#include <string.h>

// Include Application Headers
#include "test_cluster.h"
#include "test_host.h"

// Include Target Specific Headers
#include "soc.h"
#include "test.h"

// Include Driver Headers
#include "trampoline_snitchCluster.h"

// Include Runtime Headers
#include "snrt.h"

/**
 * @brief Interrupt handler for the cluster, which clears the interrupt flag for the current hart.
 *
 * @warning Stack, thread and global pointer might not yet be set up!
 */
__attribute__((naked)) void clusterInterruptHandler() {
    _SET_CLUSTER_BUSY();
    _SETUP_GP();

    asm volatile(
        // Load mhartid CSR into t0
        "csrr t0, mhartid\n"

        // Load clint base address into t1
        "la t1, __base_clint\n"

        // Calculate the interrupt target address: t1 = t1 + (t0 * 4)
        "slli t0, t0, 2\n"
        "add t1, t1, t0\n"
        // Store 0 to the interrupt target address
        "sw zero, 0(t1)\n"
        "ret"
        :            // No outputs
        :            // No inputs
        : "t0", "t1" // Declare clobbered registers
    );
}

#define BUF_SIZE 32768

volatile snrt_barrier_t g_intercluster_barrier = {
    .cnt = 0,
    .iteration = 0,
};

/**
 * @brief Main function of the cluster test.
 *
 * @return int Return 0 if the test was successful, -1 otherwise.
 */
int32_t dma_l2_test(void *args) {
    test_cluster_args_t *test_args = (test_cluster_args_t *)args;
    test_cluster_result_t *test_retVal = (test_cluster_result_t *)(test_args->result);
    dma_l2_test_args_t *user_args = (dma_l2_test_args_t *)(test_args->args);

    /*
     * Initialize the Snitch runtime.
     */
    snrt_init();

    /*
    * Use the cluster to copy larger data from L2 to L2
    */
    if (user_args -> init_size_l2 != user_args -> size_bytes) {
        // In this test, the 4th cluster always has to work and will be used to initialize the buffer rapidly
        // using the wide interface.
        if (snrt_cluster_idx() == 4 && snrt_is_dm_core()) {

            printf("Initializing L2 buffer: init size: %zu bytes, target size: %zu bytes\n",
               user_args->init_size_l2, user_args->size_bytes);

            uint32_t num_writes = user_args->size_bytes / user_args->init_size_l2;
            for (int i = 1; i < num_writes; i++) {
                // Start DMA write from L1 to L2
                snrt_dma_start_1d((void *)((uintptr_t)user_args->pointer_l2 +
                                                        i * user_args->init_size_l2),
                                (void *)((uintptr_t)user_args->pointer_l2), user_args->init_size_l2);
            }
            // Wait for DMA transfer to complete
            snrt_dma_wait_all();

            printf("Initializing L2 buffer done.\n");
        }
        snrt_cluster_hw_barrier();
        if (snrt_is_dm_core()) {
            snrt_partial_barrier((snrt_barrier_t *)&g_intercluster_barrier, user_args->clusters);
        }
        snrt_cluster_hw_barrier();
    }

    /*
     * DM core (data master) performs data setup and L1 allocation. This keeps
     * high-latency operations out of the compute cores and centralizes
     * memory management.
     */
    if (snrt_is_dm_core()) {
        void *buffer_l1 = snrt_l1_alloc(user_args->size_bytes);

        // Allocating L1 buffer
        if (buffer_l1 == NULL) {
            printf("Error: Unable to allocate L1 buffer of size %zu bytes\n",
                   user_args->size_bytes);
            test_retVal->errors = -1;
            return -1;
        }

        if (user_args->direction == DMA_READ_L2) {
            // printf("Starting DMA Read L2 Test (%zu*%d bytes, %p -> %p)...\n", user_args->size_bytes,
            //        test_args->repetitions, user_args->pointer_l2, buffer_l1);
        } else {
            printf("Starting DMA Write L2 Test (%zu*%d bytes, %p -> %p)...\n",
                   user_args->size_bytes, test_args->repetitions, buffer_l1, user_args->pointer_l2);

            // Initialize L1 buffer with some data for write test
            for (size_t i = 0; i < user_args->size_bytes; i++) {
                ((uint8_t *)buffer_l1)[i] = (uint8_t)(i & 0xFF);
            }
        }

        uint32_t start_cycles = 0, end_cycles = 0;
        uint32_t start_instructions = 0, end_instructions = 0;

        // for (volatile int i = 0; i < 2; i++) {
            if (user_args->direction == DMA_READ_L2) {
                start_cycles = snrt_mcycle();
                start_instructions = snrt_minstret();
                for (int i = 0; i < 200; i++) {
                    // Start DMA read from L2 to L1
                    snrt_dma_start_1d(buffer_l1, user_args->pointer_l2, user_args->size_bytes);
                    // Wait for DMA transfer to complete
                    snrt_dma_wait_all();
                }
                end_cycles = snrt_mcycle();
                end_instructions = snrt_minstret();
            } else {
                start_cycles = snrt_mcycle();
                start_instructions = snrt_minstret();
                for (int i = 0; i < test_args->repetitions; i++) {
                    // Start DMA write from L1 to L2
                    snrt_dma_start_1d(user_args->pointer_l2, buffer_l1, user_args->size_bytes);
                    // Wait for DMA transfer to complete
                    snrt_dma_wait_all();
                }
                end_cycles = snrt_mcycle();
                end_instructions = snrt_minstret();
            }
        // }

        printf("DMA L2 cycles = %u\n", end_cycles - start_cycles);
        printf("DMA L2 instructions = %u\n", end_instructions - start_instructions);

        float bytes_transferred = (float)(user_args->size_bytes) * 200;
        float bandwidth = bytes_transferred / (float)(end_cycles - start_cycles); // bytes per cycle
        printf("DMA L2 Bandwidth = %.2f bytes/cycle\n", bandwidth);
        printf("DMA L2 Bandwidth = %.2f bits/cycle\n", bandwidth * 8);

        test_retVal->errors = 0;
        test_retVal->ops_per_cycle = (uint32_t)(bandwidth * 1e6); // scaled for reporting
        test_retVal->runtime_cycles = end_cycles - start_cycles;
    }

    snrt_cluster_hw_barrier();

    return 0;
}
