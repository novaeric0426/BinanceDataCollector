/**
* kline_reader.c
* 
* A tool to read and display binary kline/candlestick data collected by binance_data_collector
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

// Must match the structure in binance_data_collector.c
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
} kline_record_t;

// Convert Unix timestamp to human-readable date
void format_timestamp(int64_t timestamp, char *buffer, size_t size) {
    time_t time_val = timestamp / 1000; // Convert from milliseconds to seconds
    struct tm *tm_info = localtime(&time_val);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <kline_file> [count]\n", argv[0]);
        printf("  kline_file - Path to binary kline file\n");
        printf("  count      - Number of records to display (default: all)\n");
        return 1;
    }
    
    const char *file_path = argv[1];
    int max_count = -1; // Default to all records
    
    if (argc >= 3) {
        max_count = atoi(argv[2]);
        if (max_count <= 0) {
            printf("Invalid count: %s\n", argv[2]);
            return 1;
        }
    }
    
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        perror("Failed to open file");
        return 1;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Calculate number of records
    size_t record_size = sizeof(kline_record_t);
    size_t record_count = file_size / record_size;
    
    printf("File: %s\n", file_path);
    printf("File size: %ld bytes\n", file_size);
    printf("Record size: %zu bytes\n", record_size);
    printf("Total records: %zu\n\n", record_count);
    
    // Display header
    printf("%-24s %-24s %-12s %-12s %-12s %-12s %-15s %-10s %s\n", 
           "Open Time", "Close Time", "Open", "Close", "High", "Low", 
           "Volume", "Trades", "Final");
    printf("%-24s %-24s %-12s %-12s %-12s %-12s %-15s %-10s %s\n", 
           "------------------------", "------------------------", 
           "------------", "------------", "------------", "------------", 
           "---------------", "----------", "-----");
    
    // Read and display records
    int count = 0;
    kline_record_t record;
    char open_time_str[32], close_time_str[32];
    
    while (fread(&record, record_size, 1, file) == 1) {
        // Check if we've reached the maximum count
        if (max_count > 0 && count >= max_count) {
            break;
        }
        
        // Format timestamps
        format_timestamp(record.open_time, open_time_str, sizeof(open_time_str));
        format_timestamp(record.close_time, close_time_str, sizeof(close_time_str));
        
        // Display record
        printf("%-24s %-24s %-12.8f %-12.8f %-12.8f %-12.8f %-15.8f %-10lld %s\n", 
               open_time_str, close_time_str, 
               record.open_price, record.close_price, 
               record.high_price, record.low_price, 
               record.volume, (long long)record.num_trades, 
               record.is_final ? "Yes" : "No");
        
        count++;
    }
    
    printf("\nDisplayed %d out of %zu records\n", count, record_count);
    
    fclose(file);
    return 0;
}