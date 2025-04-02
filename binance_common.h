/**
* binance_common.h
* 
* Common definitions for Binance data collector and related applications
*/

#ifndef BINANCE_COMMON_H
#define BINANCE_COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>

// Define the maximum number of symbols to track
#define MAX_SYMBOLS 10
#define MAX_SYMBOL_LENGTH 16

// Define buffer sizes
#define MAX_PAYLOAD 65536                     // 64KB max for a single message
#define MAX_RECORDS_PER_SYMBOL 100            // Maximum records to store per symbol in shared memory

// Define shared memory size
#define SHM_SIZE (64 * 1024 * 1024)          // 64MB shared memory

// Define log intervals
#define LOG_INTERVAL_SEC 5                    // Log stats every 5 seconds
#define SHM_UPDATE_INTERVAL_MS 500            // Update shared memory every 500ms

// Trading record structure (packed to minimize memory usage)
typedef struct __attribute__((packed)) {
    int64_t event_time;     // Event timestamp
    int64_t trade_time;     // Trade timestamp
    double price;           // Trade price
    double quantity;        // Trade quantity
    int64_t trade_id;       // Trade ID
    uint8_t is_buyer_maker; // 1 if buyer is maker, 0 otherwise
} trade_record_t;           // 33 bytes without padding

// Kline/candlestick record structure
typedef struct __attribute__((packed)) {
    int64_t open_time;      // Kline open time
    int64_t close_time;     // Kline close time
    double open_price;      // Open price
    double close_price;     // Close price
    double high_price;      // High price
    double low_price;       // Low price
    double volume;          // Base asset volume
    int64_t num_trades;     // Number of trades
    uint8_t is_final;       // Indicates if this kline is final
} kline_record_t;           // 57 bytes without padding

// Data type enum
typedef enum {
    DATA_TYPE_TRADE = 1,
    DATA_TYPE_KLINE = 2
} data_type_t;

// Message header structure for the shared memory
typedef struct __attribute__((packed)) {
    data_type_t type;       // Type of data (trade or kline)
    uint32_t length;        // Length of data
    int64_t timestamp;      // System timestamp when received
    char symbol[MAX_SYMBOL_LENGTH]; // Symbol name
} message_header_t;

// Shared memory structure
typedef struct __attribute__((aligned(64))) {
    atomic_uint_fast64_t write_counter;  // Number of writes to shared memory
    atomic_uint_fast64_t last_update_time; // Last update timestamp
    size_t data_offset;        // Offset where actual data begins
    size_t buffer_size;        // Size of each symbol's buffer area
    size_t symbol_count;       // Number of active symbols
    char symbols[MAX_SYMBOLS][MAX_SYMBOL_LENGTH]; // Symbol names
    // Data buffers follow this header in memory
} shared_memory_header_t;

// Symbol data structure for collecting data
typedef struct {
    char name[MAX_SYMBOL_LENGTH];
    FILE *trade_file;       // File for storing trade data
    FILE *kline_file;       // File for storing kline data
    pthread_mutex_t mutex;  // Mutex for thread safety
    
    // Recent data storage for shared memory
    struct {
        // Circular buffer for recent trades
        struct {
            trade_record_t records[MAX_RECORDS_PER_SYMBOL];
            message_header_t headers[MAX_RECORDS_PER_SYMBOL];
            size_t count;
            size_t next_index;
        } trades;
        
        // Circular buffer for recent klines
        struct {
            kline_record_t records[MAX_RECORDS_PER_SYMBOL];
            message_header_t headers[MAX_RECORDS_PER_SYMBOL];
            size_t count;
            size_t next_index;
        } klines;
    } recent_data;
    
    atomic_uint_fast64_t trade_count;
    atomic_uint_fast64_t kline_count;
    atomic_uint_fast64_t message_count;  // Number of messages processed
    atomic_uint_fast64_t bytes_processed; // Number of bytes processed
} symbol_data_t;

#endif /* BINANCE_COMMON_H */