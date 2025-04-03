/**
* binance_shared_memory_reader.c
* 
* Reads and displays data from the shared memory created by binance_data_collector
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <pthread.h>

// Include our common header file
#include "binance_common.h"

// Global variables
static volatile int force_exit = 0;
static int shm_fd = -1;
static void *shared_memory = NULL;
static shared_memory_header_t *shm_header = NULL;
static int max_records = 10; // Default max records to display

// Function declarations
void signal_handler(int sig);
void print_usage(const char *program_name);
void print_shared_memory_info();
void display_symbol_data(const char *symbol);
void display_all_symbols_data();
void print_formatted_time(int64_t timestamp);

/**
 * Signal handler for clean exit
 */
void signal_handler(int sig) {
    force_exit = 1;
}

/**
 * Print usage information
 */
void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -s SYMBOL    Display data for specific symbol (e.g., BTCUSDT)\n");
    printf("  -c           Continuous mode: update display periodically\n");
    printf("  -i INTERVAL  Update interval in milliseconds for continuous mode (default: 1000)\n");
    printf("  -n COUNT     Maximum number of records to display per symbol (default: 10)\n");
    printf("  -h           Display this help message\n");
}

/**
 * Print formatted timestamp
 */
void print_formatted_time(int64_t timestamp) {
    time_t time_val = timestamp / 1000;  // Convert from milliseconds to seconds
    struct tm *tm_info = localtime(&time_val);
    
    char buffer[26];
    strftime(buffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    printf("%s.%03lld", buffer, timestamp % 1000);
}

/**
 * Print shared memory information
 */
void print_shared_memory_info() {
    if (!shm_header) return;
    
    time_t current_time = time(NULL);
    time_t last_update = atomic_load(&shm_header->last_update_time);
    int seconds_since_update = (int)difftime(current_time, last_update);
    
    printf("=== Binance Market Data Shared Memory ===\n");
    printf("Last update: %s", ctime(&last_update));
    printf("Time since last update: %d seconds\n", seconds_since_update);
    printf("Write counter: %llu\n", atomic_load(&shm_header->write_counter));
    printf("Symbol count: %zu\n", shm_header->symbol_count);
    printf("Symbols: ");
    
    for (size_t i = 0; i < shm_header->symbol_count; i++) {
        printf("%s ", shm_header->symbols[i]);
    }
    printf("\n\n");
    
    printf("Shared memory layout:\n");
    printf("  Header size: %zu bytes\n", sizeof(shared_memory_header_t));
    printf("  Data offset: %zu bytes\n", shm_header->data_offset);
    printf("  Buffer size per symbol: %zu bytes\n", shm_header->buffer_size);
    printf("  Total shared memory size: %d bytes\n", SHM_SIZE);
}

/**
 * Display data for a specific symbol from shared memory
 */
void display_symbol_data(const char *symbol) {
    if (!shm_header) return;
    
    // Find the symbol index
    int symbol_idx = -1;
    for (size_t i = 0; i < shm_header->symbol_count; i++) {
        if (strcasecmp(shm_header->symbols[i], symbol) == 0) {
            symbol_idx = i;
            break;
        }
    }
    
    if (symbol_idx == -1) {
        printf("Symbol %s not found in shared memory\n", symbol);
        return;
    }
    
    // Calculate offset for this symbol
    size_t symbol_offset = shm_header->data_offset + symbol_idx * shm_header->buffer_size;
    
    // Check if the offset is within the shared memory
    if (symbol_offset >= SHM_SIZE) {
        printf("Symbol %s: Offset %zu is outside shared memory bounds\n", symbol, symbol_offset);
        return;
    }
    
    // Read the size field (if it exists)
    if (symbol_offset + sizeof(size_t) > SHM_SIZE) {
        printf("Symbol %s: Can't read size field, offset %zu + %zu exceeds shared memory size\n", 
              symbol, symbol_offset, sizeof(size_t));
        return;
    }
    
    size_t data_size = *((size_t *)((char *)shared_memory + symbol_offset));
    printf("Symbol %s: Data size: %zu bytes\n", symbol, data_size);
    
    // Check if the data size is reasonable
    if (data_size == 0) {
        printf("No data available for symbol %s\n", symbol);
        return;
    }
    
    if (data_size > shm_header->buffer_size - sizeof(size_t)) {
        printf("Warning: Data size (%zu) is larger than available buffer size (%zu), might be corrupt\n",
              data_size, shm_header->buffer_size - sizeof(size_t));
        return;
    }
    
    // Skip the size field
    symbol_offset += sizeof(size_t);
    
    // Try to parse and display data
    printf("Data for symbol %s:\n", symbol);
    
    // Loop through the data to find message headers
    size_t offset = 0;
    int record_count = 0;
    
    while (offset < data_size && record_count < max_records) {
        // Ensure we have enough data for a message header
        if (offset + sizeof(message_header_t) > data_size) {
            printf("Incomplete message header at offset %zu\n", offset);
            break;
        }
        
        // Read message header
        message_header_t *header = (message_header_t *)((char *)shared_memory + symbol_offset + offset);
        offset += sizeof(message_header_t);
        
        // Verify symbol name in header
        if (strcasecmp(header->symbol, symbol) != 0) {
            printf("Warning: Message header has mismatched symbol: %s (expected %s)\n", 
                  header->symbol, symbol);
            // Try to recover by searching for the next valid header
            offset = (offset + 7) & ~7; // Align to 8-byte boundary
            continue;
        }
        
        // Process based on data type
        if (header->type == DATA_TYPE_TRADE) {
            // Ensure we have enough data for a trade record
            if (offset + header->length > data_size || header->length != sizeof(trade_record_t)) {
                printf("Invalid trade record length %u at offset %zu\n", header->length, offset);
                break;
            }
            
            // Read trade record
            trade_record_t *trade = (trade_record_t *)((char *)shared_memory + symbol_offset + offset);
            
            printf("[TRADE] Time: ");
            print_formatted_time(trade->trade_time);
            printf(", Event time: ");
            print_formatted_time(trade->event_time);
            printf("\n        Price: %.8f, Qty: %.8f, TradeID: %lld, BuyerMaker: %d\n",
                  trade->price, trade->quantity, trade->trade_id, trade->is_buyer_maker);
            
            offset += header->length;
            record_count++;
        } else if (header->type == DATA_TYPE_KLINE) {
            // Ensure we have enough data for a kline record
            if (offset + header->length > data_size || header->length != sizeof(kline_record_t)) {
                printf("Invalid kline record length %u at offset %zu\n", header->length, offset);
                break;
            }
            
            // Read kline record
            kline_record_t *kline = (kline_record_t *)((char *)shared_memory + symbol_offset + offset);
            
            printf("[KLINE] Open time: ");
            print_formatted_time(kline->open_time);
            printf(", Close time: ");
            print_formatted_time(kline->close_time);
            printf("\n        OHLC: %.8f, %.8f, %.8f, %.8f, Vol: %.8f, Trades: %lld, Final: %d\n",
                  kline->open_price, kline->high_price, kline->low_price, kline->close_price,
                  kline->volume, kline->num_trades, kline->is_final);
            
            offset += header->length;
            record_count++;
        } else {
            printf("Unknown data type %d at offset %zu\n", header->type, offset - sizeof(message_header_t));
            // Try to recover by searching for the next valid header
            offset = (offset + 7) & ~7; // Align to 8-byte boundary
        }
    }
    
    if (record_count == 0) {
        printf("No valid records found for symbol %s\n", symbol);
    } else if (offset < data_size) {
        printf("... more records available (showing %d of %d)\n", 
              record_count, (int)(data_size / (sizeof(message_header_t) + sizeof(trade_record_t))));
    }
    
    printf("\n");
}

/**
 * Display data for all symbols in shared memory
 */
void display_all_symbols_data() {
    if (!shm_header) return;
    
    for (size_t i = 0; i < shm_header->symbol_count; i++) {
        display_symbol_data(shm_header->symbols[i]);
    }
}

/**
 * Main function
 */
int main(int argc, char **argv) {
    int c;
    int continuous = 0;
    int interval_ms = 1000;  // Default 1 second
    char *specific_symbol = NULL;
    
    // Register signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Parse command line arguments
    while ((c = getopt(argc, argv, "s:ci:n:h")) != -1) {
        switch (c) {
            case 's':
                specific_symbol = optarg;
                break;
            case 'c':
                continuous = 1;
                break;
            case 'i':
                interval_ms = atoi(optarg);
                if (interval_ms < 100) interval_ms = 100;
                break;
            case 'n':
                max_records = atoi(optarg);
                if (max_records < 1) max_records = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return c == 'h' ? 0 : 1;
        }
    }
    
    // Open the shared memory
    shm_fd = shm_open("/binance_market_data", O_RDONLY, 0666);
    if (shm_fd == -1) {
        perror("Failed to open shared memory");
        fprintf(stderr, "Make sure the binance_data_collector is running\n");
        return 1;
    }
    
    // Map the shared memory
    shared_memory = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (shared_memory == MAP_FAILED) {
        perror("Failed to map shared memory");
        close(shm_fd);
        return 1;
    }
    
    // Get the shared memory header
    shm_header = (shared_memory_header_t *)shared_memory;
    
    // Display mode
    if (continuous) {
        printf("Continuous mode: Press Ctrl+C to exit\n");
        while (!force_exit) {
            printf("\033[2J\033[H");  // Clear screen and move cursor to top
            print_shared_memory_info();
            
            if (specific_symbol) {
                display_symbol_data(specific_symbol);
            } else {
                display_all_symbols_data();
            }
            
            usleep(interval_ms * 1000);
        }
    } else {
        print_shared_memory_info();
        
        if (specific_symbol) {
            display_symbol_data(specific_symbol);
        } else {
            display_all_symbols_data();
        }
    }
    
    // Clean up
    if (shared_memory && shared_memory != MAP_FAILED) {
        munmap(shared_memory, SHM_SIZE);
    }
    
    if (shm_fd != -1) {
        close(shm_fd);
    }
    
    return 0;
}