/**
* binance_data_collector.c
* 
* Binance WebSocket data collector using libwebsockets
* This program connects to Binance WebSocket API and stores market data
* both on disk and in shared memory for other processes to access.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <pthread.h>
#include <libwebsockets.h>
#include <json-c/json.h>
#include <immintrin.h> // For AVX instructions
#include <limits.h>

// Include our common header file
#include "binance_common.h"

// Global variables
static struct lws_context *lws_context = NULL;
static volatile int force_exit = 0;
static symbol_data_t symbols[MAX_SYMBOLS];
static size_t symbol_count = 0;
static char *output_dir = "./data";
static int shm_fd = -1;
static void *shared_memory = NULL;
static shared_memory_header_t *shm_header = NULL;
static pthread_t stats_thread;
static pthread_t shm_update_thread;
static time_t last_update_time = 0;

// Forward declarations
void init_symbol_data(symbol_data_t *symbol);
void *stats_thread_func(void *arg);
void *shm_update_thread_func(void *arg);
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *in, size_t len);
void handle_aggTrade(json_object *root, const char *symbol);
void handle_kline(json_object *root, const char *symbol);
int init_shared_memory();
void cleanup_shared_memory();
void update_shared_memory();
void signal_handler(int sig);

// Protocol definition
static const struct lws_protocols protocols[] = {
    {
        "binance-stream",           // name
        ws_callback,                 // callback
        0,                           // per_session_data_size
        MAX_PAYLOAD,                 // rx_buffer_size
    },
    { NULL, NULL, 0, 0 }             // terminator
};

/**
 * Initialize a symbol's data structure
 */
void init_symbol_data(symbol_data_t *symbol) {
    pthread_mutex_init(&symbol->mutex, NULL);
    
    // Initialize recent data storage
    memset(&symbol->recent_data, 0, sizeof(symbol->recent_data));
    
    atomic_init(&symbol->trade_count, 0);
    atomic_init(&symbol->kline_count, 0);
    atomic_init(&symbol->message_count, 0);
    atomic_init(&symbol->bytes_processed, 0);
}

/**
 * Statistics thread function
 * Periodically logs statistics about data collection
 */
void *stats_thread_func(void *arg) {
    while (!force_exit) {
        // Sleep for the log interval
        sleep(LOG_INTERVAL_SEC);
        
        // Print statistics for each symbol
        printf("\n--- Statistics (as of %s) ---\n", ctime(&(time_t){time(NULL)}));
        printf("Symbol  | Trade Count | Kline Count | Messages/sec | MB/sec   \n");
        printf("--------|-------------|-------------|--------------|----------\n");
        
        for (size_t i = 0; i < symbol_count; i++) {
            uint64_t trade_count = atomic_load(&symbols[i].trade_count);
            uint64_t kline_count = atomic_load(&symbols[i].kline_count);
            uint64_t message_count = atomic_load(&symbols[i].message_count);
            uint64_t bytes_processed = atomic_load(&symbols[i].bytes_processed);
            
            // Calculate message rate and data rate
            static uint64_t prev_message_counts[MAX_SYMBOLS] = {0};
            static uint64_t prev_bytes_processed[MAX_SYMBOLS] = {0};
            
            uint64_t msg_diff = message_count - prev_message_counts[i];
            double msg_rate = (double)msg_diff / LOG_INTERVAL_SEC;
            
            uint64_t bytes_diff = bytes_processed - prev_bytes_processed[i];
            double mb_rate = (double)bytes_diff / (1024 * 1024) / LOG_INTERVAL_SEC;
            
            printf("%-8s| %-11llu | %-11llu | %-12.2f | %-10.2f\n", 
                   symbols[i].name, trade_count, kline_count, msg_rate, mb_rate);
            
            prev_message_counts[i] = message_count;
            prev_bytes_processed[i] = bytes_processed;
        }
        
        // Print shared memory stats
        if (shm_header) {
            printf("\nShared Memory: Write counter: %llu, Last update: %s", 
                   atomic_load(&shm_header->write_counter),
                   ctime((time_t*)&shm_header->last_update_time));
            
            // Print recent records count for each symbol
            printf("Recent records in memory:\n");
            printf("Symbol  | Trades | Klines \n");
            printf("--------|--------|--------\n");
            
            for (size_t i = 0; i < symbol_count; i++) {
                pthread_mutex_lock(&symbols[i].mutex);
                printf("%-8s| %-6zu | %-6zu\n", 
                       symbols[i].name, 
                       symbols[i].recent_data.trades.count,
                       symbols[i].recent_data.klines.count);
                pthread_mutex_unlock(&symbols[i].mutex);
            }
        }
    }
    
    return NULL;
}

/**
 * Thread function to update shared memory periodically
 */
void *shm_update_thread_func(void *arg) {
    while (!force_exit) {
        // Update shared memory with the latest data
        update_shared_memory();
        
        // Sleep for the update interval
        usleep(SHM_UPDATE_INTERVAL_MS * 1000);
    }
    
    return NULL;
}

/**
 * WebSocket callback function
 */
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            fprintf(stderr, "WebSocket connection established\n");
            break;
        
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            // Parse JSON message
            json_object *root = json_tokener_parse((const char *)in);
            if (!root) {
                fprintf(stderr, "Failed to parse JSON message\n");
                break;
            }
            
            // Extract stream name and data
            json_object *stream_obj;
            if (json_object_object_get_ex(root, "stream", &stream_obj)) {
                const char *stream = json_object_get_string(stream_obj);
                
                // Parse stream to extract symbol and type
                char symbol[MAX_SYMBOL_LENGTH] = {0};
                int i = 0;
                while (stream[i] != '@' && stream[i] != '\0' && i < MAX_SYMBOL_LENGTH - 1) {
                    symbol[i] = toupper(stream[i]);
                    i++;
                }
                symbol[i] = '\0';
                
                // Get data object
                json_object *data_obj;
                if (json_object_object_get_ex(root, "data", &data_obj)) {
                    // Process based on stream type
                    if (strstr(stream, "@aggTrade")) {
                        handle_aggTrade(data_obj, symbol);
                    } else if (strstr(stream, "@kline")) {
                        handle_kline(data_obj, symbol);
                    }
                }
            }
            
            json_object_put(root);
            break;
        }
        
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            fprintf(stderr, "WebSocket connection error: %s\n", 
                   in ? (char *)in : "(null)");
            break;
        
        case LWS_CALLBACK_CLOSED:
            fprintf(stderr, "WebSocket connection closed\n");
            break;
        
        default:
            break;
    }
    
    return 0;
}

/**
 * Handle aggTrade message
 */
void handle_aggTrade(json_object *root, const char *symbol) {
    // Find the symbol in our array
    int symbol_idx = -1;
    for (size_t i = 0; i < symbol_count; i++) {
        if (strcmp(symbols[i].name, symbol) == 0) {
            symbol_idx = i;
            break;
        }
    }
    
    if (symbol_idx == -1) {
        fprintf(stderr, "Received data for unknown symbol: %s\n", symbol);
        return;
    }
    
    // Extract trade data
    trade_record_t record;
    
    json_object *obj;
    if (json_object_object_get_ex(root, "E", &obj)) 
        record.event_time = json_object_get_int64(obj);
    
    if (json_object_object_get_ex(root, "T", &obj)) 
        record.trade_time = json_object_get_int64(obj);
    
    if (json_object_object_get_ex(root, "p", &obj)) 
        record.price = json_object_get_double(obj);
    
    if (json_object_object_get_ex(root, "q", &obj)) 
        record.quantity = json_object_get_double(obj);
    
    if (json_object_object_get_ex(root, "a", &obj)) 
        record.trade_id = json_object_get_int64(obj);
    
    if (json_object_object_get_ex(root, "m", &obj)) 
        record.is_buyer_maker = json_object_get_boolean(obj) ? 1 : 0;
    
    // Write directly to file
    if (fwrite(&record, sizeof(record), 1, symbols[symbol_idx].trade_file) != 1) {
        fprintf(stderr, "Failed to write trade data to file for symbol %s\n", symbols[symbol_idx].name);
    } else {
        // Update statistics
        atomic_fetch_add(&symbols[symbol_idx].trade_count, 1);
        atomic_fetch_add(&symbols[symbol_idx].message_count, 1);
        atomic_fetch_add(&symbols[symbol_idx].bytes_processed, sizeof(record));
        
        // Also store in memory for shared memory updates
        pthread_mutex_lock(&symbols[symbol_idx].mutex);
        
        // Get the next position in the circular buffer
        size_t idx = symbols[symbol_idx].recent_data.trades.next_index;
        
        // Store the record
        symbols[symbol_idx].recent_data.trades.records[idx] = record;
        
        // Create and store the header
        message_header_t header;
        header.type = DATA_TYPE_TRADE;
        header.length = sizeof(record);
        header.timestamp = time(NULL);
        strncpy(header.symbol, symbol, MAX_SYMBOL_LENGTH - 1);
        header.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
        
        symbols[symbol_idx].recent_data.trades.headers[idx] = header;
        
        // Update the index for next write
        symbols[symbol_idx].recent_data.trades.next_index = (idx + 1) % MAX_RECORDS_PER_SYMBOL;
        
        // Update count if we haven't filled the buffer yet
        if (symbols[symbol_idx].recent_data.trades.count < MAX_RECORDS_PER_SYMBOL) {
            symbols[symbol_idx].recent_data.trades.count++;
        }
        
        pthread_mutex_unlock(&symbols[symbol_idx].mutex);
    }
    
    // Flush to ensure data is written
    fflush(symbols[symbol_idx].trade_file);
}

/**
 * Handle kline message
 */
void handle_kline(json_object *root, const char *symbol) {
    // Find the symbol in our array
    int symbol_idx = -1;
    for (size_t i = 0; i < symbol_count; i++) {
        if (strcmp(symbols[i].name, symbol) == 0) {
            symbol_idx = i;
            break;
        }
    }
    
    if (symbol_idx == -1) {
        fprintf(stderr, "Received data for unknown symbol: %s\n", symbol);
        return;
    }
    
    // Get kline object
    json_object *k_obj;
    if (!json_object_object_get_ex(root, "k", &k_obj)) {
        fprintf(stderr, "Failed to find kline object in message\n");
        return;
    }
    
    // Extract kline data
    kline_record_t record;
    
    json_object *obj;
    if (json_object_object_get_ex(k_obj, "t", &obj)) 
        record.open_time = json_object_get_int64(obj);
    
    if (json_object_object_get_ex(k_obj, "T", &obj)) 
        record.close_time = json_object_get_int64(obj);
    
    if (json_object_object_get_ex(k_obj, "o", &obj)) 
        record.open_price = json_object_get_double(obj);
    
    if (json_object_object_get_ex(k_obj, "c", &obj)) 
        record.close_price = json_object_get_double(obj);
    
    if (json_object_object_get_ex(k_obj, "h", &obj)) 
        record.high_price = json_object_get_double(obj);
    
    if (json_object_object_get_ex(k_obj, "l", &obj)) 
        record.low_price = json_object_get_double(obj);
    
    if (json_object_object_get_ex(k_obj, "v", &obj)) 
        record.volume = json_object_get_double(obj);
    
    if (json_object_object_get_ex(k_obj, "n", &obj)) 
        record.num_trades = json_object_get_int64(obj);
    
    if (json_object_object_get_ex(k_obj, "x", &obj)) 
        record.is_final = json_object_get_boolean(obj) ? 1 : 0;
    
    // Write directly to file
    if (fwrite(&record, sizeof(record), 1, symbols[symbol_idx].kline_file) != 1) {
        fprintf(stderr, "Failed to write kline data to file for symbol %s\n", symbols[symbol_idx].name);
    } else {
        // Update statistics
        atomic_fetch_add(&symbols[symbol_idx].kline_count, 1);
        atomic_fetch_add(&symbols[symbol_idx].message_count, 1);
        atomic_fetch_add(&symbols[symbol_idx].bytes_processed, sizeof(record));
        
        // Also store in memory for shared memory updates
        pthread_mutex_lock(&symbols[symbol_idx].mutex);
        
        // Get the next position in the circular buffer
        size_t idx = symbols[symbol_idx].recent_data.klines.next_index;
        
        // Store the record
        symbols[symbol_idx].recent_data.klines.records[idx] = record;
        
        // Create and store the header
        message_header_t header;
        header.type = DATA_TYPE_KLINE;
        header.length = sizeof(record);
        header.timestamp = time(NULL);
        strncpy(header.symbol, symbol, MAX_SYMBOL_LENGTH - 1);
        header.symbol[MAX_SYMBOL_LENGTH - 1] = '\0';
        
        symbols[symbol_idx].recent_data.klines.headers[idx] = header;
        
        // Update the index for next write
        symbols[symbol_idx].recent_data.klines.next_index = (idx + 1) % MAX_RECORDS_PER_SYMBOL;
        
        // Update count if we haven't filled the buffer yet
        if (symbols[symbol_idx].recent_data.klines.count < MAX_RECORDS_PER_SYMBOL) {
            symbols[symbol_idx].recent_data.klines.count++;
        }
        
        pthread_mutex_unlock(&symbols[symbol_idx].mutex);
    }
    
    // Flush to ensure data is written
    fflush(symbols[symbol_idx].kline_file);
}

/**
 * Initialize shared memory
 * Returns 0 on success, -1 on failure
 */
int init_shared_memory() {
    // Create/open shared memory
    shm_fd = shm_open("/binance_market_data", O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        return -1;
    }
    
    // Set the size of the shared memory
    if (ftruncate(shm_fd, SHM_SIZE) == -1) {
        perror("ftruncate failed");
        close(shm_fd);
        shm_unlink("/binance_market_data");
        return -1;
    }
    
    // Map the shared memory
    shared_memory = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_memory == MAP_FAILED) {
        perror("mmap failed");
        close(shm_fd);
        shm_unlink("/binance_market_data");
        return -1;
    }
    
    // Initialize the shared memory header
    shm_header = (shared_memory_header_t *)shared_memory;
    atomic_init(&shm_header->write_counter, 0);
    atomic_init(&shm_header->last_update_time, time(NULL));
    shm_header->data_offset = sizeof(shared_memory_header_t);
    shm_header->buffer_size = (SHM_SIZE - shm_header->data_offset) / MAX_SYMBOLS;
    shm_header->symbol_count = symbol_count;
    
    // Copy symbol names to shared memory
    for (size_t i = 0; i < symbol_count; i++) {
        strncpy(shm_header->symbols[i], symbols[i].name, MAX_SYMBOL_LENGTH - 1);
        shm_header->symbols[i][MAX_SYMBOL_LENGTH - 1] = '\0';
    }
    
    printf("Shared memory initialized at /binance_market_data (%d MB, %zu MB per symbol)\n", 
           SHM_SIZE / (1024 * 1024), shm_header->buffer_size / (1024 * 1024));
    
    return 0;
}

/**
 * Clean up shared memory
 */
void cleanup_shared_memory() {
    if (shared_memory && shared_memory != MAP_FAILED) {
        munmap(shared_memory, SHM_SIZE);
        shared_memory = NULL;
    }
    
    if (shm_fd != -1) {
        close(shm_fd);
        shm_unlink("/binance_market_data");
        shm_fd = -1;
    }
}

/**
 * Update shared memory with latest data
 */
void update_shared_memory() {
    if (!shm_header) return;
    
    // Update last update time
    time_t now = time(NULL);
    atomic_store(&shm_header->last_update_time, now);
    
    // Check if we need to update the data (avoid too frequent updates)
    if (now - last_update_time < 1) {
        return;
    }
    last_update_time = now;
    
    // Update data for each symbol
    for (size_t i = 0; i < symbol_count; i++) {
        // Calculate offset for this symbol in shared memory
        size_t symbol_offset = shm_header->data_offset + i * shm_header->buffer_size;
        
        // Make sure we have room for at least the size field
        if (symbol_offset + sizeof(size_t) > SHM_SIZE) {
            fprintf(stderr, "Error: Symbol offset exceeds shared memory size\n");
            continue;
        }
        
        pthread_mutex_lock(&symbols[i].mutex);
        
        // Calculate the total data size we'll write
        size_t trade_data_size = symbols[i].recent_data.trades.count * 
                                 (sizeof(message_header_t) + sizeof(trade_record_t));
        
        size_t kline_data_size = symbols[i].recent_data.klines.count * 
                                 (sizeof(message_header_t) + sizeof(kline_record_t));
        
        size_t total_data_size = trade_data_size + kline_data_size;
        
        // Check if we have enough space in shared memory
        if (total_data_size > shm_header->buffer_size - sizeof(size_t)) {
            fprintf(stderr, "Warning: Not enough space in shared memory for symbol %s data\n", 
                   symbols[i].name);
            total_data_size = shm_header->buffer_size - sizeof(size_t);
        }
        
        // Write the total data size at the beginning of the symbol's area
        *((size_t *)((char *)shared_memory + symbol_offset)) = total_data_size;
        
        // Move past the size field
        size_t current_offset = symbol_offset + sizeof(size_t);
        
        // Write trade data
        size_t trades_to_write = symbols[i].recent_data.trades.count;
        size_t trade_bytes = trades_to_write * (sizeof(message_header_t) + sizeof(trade_record_t));
        
        if (trade_bytes > total_data_size) {
            trades_to_write = total_data_size / (sizeof(message_header_t) + sizeof(trade_record_t));
            trade_bytes = trades_to_write * (sizeof(message_header_t) + sizeof(trade_record_t));
        }
        
        // Write trades in chronological order (oldest to newest)
        size_t start_idx = symbols[i].recent_data.trades.count >= MAX_RECORDS_PER_SYMBOL ?
                         symbols[i].recent_data.trades.next_index : 0;
        
        for (size_t j = 0; j < trades_to_write; j++) {
            size_t idx = (start_idx + j) % MAX_RECORDS_PER_SYMBOL;
            
            // Write header
            memcpy((char *)shared_memory + current_offset, 
                   &symbols[i].recent_data.trades.headers[idx], 
                   sizeof(message_header_t));
            current_offset += sizeof(message_header_t);
            
            // Write trade record
            memcpy((char *)shared_memory + current_offset, 
                   &symbols[i].recent_data.trades.records[idx], 
                   sizeof(trade_record_t));
            current_offset += sizeof(trade_record_t);
        }
        
        // Write kline data if we have space left
        size_t remaining_space = total_data_size - trade_bytes;
        size_t klines_to_write = symbols[i].recent_data.klines.count;
        size_t kline_bytes = klines_to_write * (sizeof(message_header_t) + sizeof(kline_record_t));
        
        if (kline_bytes > remaining_space) {
            klines_to_write = remaining_space / (sizeof(message_header_t) + sizeof(kline_record_t));
            kline_bytes = klines_to_write * (sizeof(message_header_t) + sizeof(kline_record_t));
        }
        
        // Write klines in chronological order (oldest to newest)
        start_idx = symbols[i].recent_data.klines.count >= MAX_RECORDS_PER_SYMBOL ?
                  symbols[i].recent_data.klines.next_index : 0;
        
        for (size_t j = 0; j < klines_to_write; j++) {
            size_t idx = (start_idx + j) % MAX_RECORDS_PER_SYMBOL;
            
            // Write header
            memcpy((char *)shared_memory + current_offset, 
                   &symbols[i].recent_data.klines.headers[idx], 
                   sizeof(message_header_t));
            current_offset += sizeof(message_header_t);
            
            // Write kline record
            memcpy((char *)shared_memory + current_offset, 
                   &symbols[i].recent_data.klines.records[idx], 
                   sizeof(kline_record_t));
            current_offset += sizeof(kline_record_t);
        }
        
        pthread_mutex_unlock(&symbols[i].mutex);
    }
    
    // Increment write counter
    atomic_fetch_add(&shm_header->write_counter, 1);
}

/**
 * Signal handler for clean exit
 */
void signal_handler(int sig) {
    force_exit = 1;
}

/**
 * Main function for the Binance data collector
 */
int main(int argc, char **argv) {
    int ret = 0;
    struct lws_client_connect_info ccinfo = {0};
    struct lws *wsi_binance = NULL;
    const char *binance_host = "fstream.binance.com";
    const char *path = "/stream";
    int logs_stdout = LLL_USER | LLL_ERR | LLL_WARN | LLL_NOTICE;
    int c;
    char stream_path[1024] = {0};
    char **symbol_list = NULL;
    int opt_index = 0;
    
    // Define command line options
    static struct option long_options[] = {
        {"symbol", required_argument, NULL, 's'},
        {"output", required_argument, NULL, 'o'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    // Parse command line arguments
    while ((c = getopt_long(argc, argv, "s:o:h", long_options, &opt_index)) != -1) {
        switch (c) {
            case 's':
                // Parse symbol list (comma-separated)
                {
                    char *token, *str, *tofree;
                    int sym_count = 0;
                    
                    // First count how many symbols
                    tofree = str = strdup(optarg);
                    while ((token = strsep(&str, ","))) {
                        sym_count++;
                    }
                    free(tofree);
                    
                    // Allocate memory for symbol pointers
                    symbol_list = (char **)malloc(sym_count * sizeof(char *));
                    
                    // Parse again to store symbols
                    tofree = str = strdup(optarg);
                    sym_count = 0;
                    while ((token = strsep(&str, ","))) {
                        symbol_list[sym_count++] = strdup(token);
                    }
                    free(tofree);
                    
                    // Set symbol_count
                    symbol_count = sym_count;
                }
                break;
                
            case 'o':
                output_dir = strdup(optarg);
                break;
                
            case 'h':
            default:
                printf("Usage: %s [options]\n", argv[0]);
                printf("Options:\n");
                printf("  -s, --symbol=SYM1,SYM2,...  Comma-separated list of symbols (e.g., btcusdt,ethusdt)\n");
                printf("  -o, --output=DIR           Output directory for data files (default: ./data)\n");
                printf("  -h, --help                 Show this help message\n");
                return c == 'h' ? 0 : 1;
        }
    }
    
    // Verify we have at least one symbol
    if (symbol_count == 0) {
        fprintf(stderr, "Error: At least one symbol must be specified.\n");
        fprintf(stderr, "Use --help for usage information.\n");
        return 1;
    }
    
    // Limit symbol count to MAX_SYMBOLS
    if (symbol_count > MAX_SYMBOLS) {
        fprintf(stderr, "Warning: Too many symbols specified. Using only the first %d.\n", MAX_SYMBOLS);
        symbol_count = MAX_SYMBOLS;
    }
    
    // Register signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create output directory if it doesn't exist
    struct stat st = {0};
    if (stat(output_dir, &st) == -1) {
        if (mkdir(output_dir, 0755) == -1) {
            fprintf(stderr, "Error: Failed to create output directory: %s\n", output_dir);
            ret = 1;
            goto cleanup;
        }
    }
    
    // Initialize symbol data structures
    for (size_t i = 0; i < symbol_count; i++) {
        char symbol_dir[PATH_MAX];
        char trade_file_path[PATH_MAX];
        char kline_file_path[PATH_MAX];
        
        // Convert symbol to uppercase
        char *symbol = symbol_list[i];
        for (int j = 0; symbol[j]; j++) {
            symbol[j] = toupper(symbol[j]);
        }
        
        strncpy(symbols[i].name, symbol, MAX_SYMBOL_LENGTH - 1);
        symbols[i].name[MAX_SYMBOL_LENGTH - 1] = '\0';
        
        // Create symbol-specific directory
        snprintf(symbol_dir, sizeof(symbol_dir), "%s/%s", output_dir, symbols[i].name);
        if (stat(symbol_dir, &st) == -1) {
            if (mkdir(symbol_dir, 0755) == -1) {
                fprintf(stderr, "Error: Failed to create directory for symbol %s\n", symbols[i].name);
                ret = 1;
                goto cleanup;
            }
        }
        
        // Create and open output files
        snprintf(trade_file_path, sizeof(trade_file_path), 
                 "%s/%s/trades_%ld.bin", output_dir, symbols[i].name, time(NULL));
        snprintf(kline_file_path, sizeof(kline_file_path), 
                 "%s/%s/klines_%ld.bin", output_dir, symbols[i].name, time(NULL));
        
        symbols[i].trade_file = fopen(trade_file_path, "wb");
        if (!symbols[i].trade_file) {
            fprintf(stderr, "Error: Failed to open trade file for symbol %s\n", symbols[i].name);
            ret = 1;
            goto cleanup;
        }
        
        symbols[i].kline_file = fopen(kline_file_path, "wb");
        if (!symbols[i].kline_file) {
            fprintf(stderr, "Error: Failed to open kline file for symbol %s\n", symbols[i].name);
            ret = 1;
            goto cleanup;
        }
        
        // Initialize other data fields
        init_symbol_data(&symbols[i]);
        
        printf("Initialized data collection for symbol: %s\n", symbols[i].name);
    }
    
    // Initialize shared memory
    if (init_shared_memory() != 0) {
        fprintf(stderr, "Error: Failed to initialize shared memory\n");
        ret = 1;
        goto cleanup;
    }
    
    // Start statistics thread
    if (pthread_create(&stats_thread, NULL, stats_thread_func, NULL) != 0) {
        fprintf(stderr, "Error: Failed to create statistics thread\n");
        ret = 1;
        goto cleanup;
    }
    
    // Start shared memory update thread
    if (pthread_create(&shm_update_thread, NULL, shm_update_thread_func, NULL) != 0) {
        fprintf(stderr, "Error: Failed to create shared memory update thread\n");
        ret = 1;
        goto cleanup;
    }
    
    // Initialize libwebsockets
    lws_set_log_level(logs_stdout, NULL);
    
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    
    lws_context = lws_create_context(&info);
    if (!lws_context) {
        fprintf(stderr, "Error: Failed to create libwebsocket context\n");
        ret = 1;
        goto cleanup;
    }
    
    // Construct WebSocket path with streams
    strcat(stream_path, path);
    strcat(stream_path, "?streams=");
    
    // Add streams for each symbol (aggTrade and kline)
    for (size_t i = 0; i < symbol_count; i++) {
        // Convert symbol to lowercase for Binance API
        char lower_symbol[MAX_SYMBOL_LENGTH];
        strncpy(lower_symbol, symbols[i].name, sizeof(lower_symbol) - 1);
        lower_symbol[sizeof(lower_symbol) - 1] = '\0';
        
        for (int j = 0; lower_symbol[j]; j++) {
            lower_symbol[j] = tolower(lower_symbol[j]);
        }
        
        // Add aggTrade stream
        if (i > 0) strcat(stream_path, "/");
        strcat(stream_path, lower_symbol);
        strcat(stream_path, "@aggTrade");
        
        // Add kline stream for 1m interval
        strcat(stream_path, "/");
        strcat(stream_path, lower_symbol);
        strcat(stream_path, "@kline_1m");
    }
    
    printf("Connecting to WebSocket: wss://%s%s\n", binance_host, stream_path);
    
    // Connect to Binance WebSocket
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context = lws_context;
    ccinfo.address = binance_host;
    ccinfo.port = 443;
    ccinfo.path = stream_path;
    ccinfo.host = ccinfo.address;
    ccinfo.origin = ccinfo.address;
    ccinfo.protocol = protocols[0].name;
    ccinfo.ssl_connection = LCCSCF_USE_SSL;
    
    wsi_binance = lws_client_connect_via_info(&ccinfo);
    if (!wsi_binance) {
        fprintf(stderr, "Error: Failed to connect to Binance WebSocket\n");
        ret = 1;
        goto cleanup;
    }
    
    printf("Data collection started. Press Ctrl+C to exit.\n");
    
    // Event loop
    while (!force_exit) {
        lws_service(lws_context, 100);
        
        // Small sleep to avoid CPU spinning
        usleep(1000); // 1ms
    }
    
    printf("\nShutting down...\n");
    
cleanup:
    // Join threads
    if (stats_thread) {
        pthread_join(stats_thread, NULL);
    }
    
    if (shm_update_thread) {
        pthread_join(shm_update_thread, NULL);
    }
    
    // Cleanup symbol data
    for (size_t i = 0; i < symbol_count; i++) {
        pthread_mutex_destroy(&symbols[i].mutex);
        
        if (symbols[i].trade_file) {
            fclose(symbols[i].trade_file);
            symbols[i].trade_file = NULL;
        }
        
        if (symbols[i].kline_file) {
            fclose(symbols[i].kline_file);
            symbols[i].kline_file = NULL;
        }
    }
    
    // Free symbol list
    if (symbol_list) {
        for (size_t i = 0; i < symbol_count; i++) {
            free(symbol_list[i]);
        }
        free(symbol_list);
    }
    
    // Cleanup libwebsockets
    if (lws_context) {
        lws_context_destroy(lws_context);
        lws_context = NULL;
    }
    
    // Cleanup shared memory
    cleanup_shared_memory();
    
    printf("Cleanup complete. Exiting.\n");
    return ret;
}