/**
* trade_reader.c
* 
* A tool to read and display binary trade data collected by binance_data_collector
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

// Must match the structure in binance_data_collector.c
typedef struct __attribute__((packed)) {
    int64_t event_time;     // Event timestamp
    int64_t trade_time;     // Trade timestamp
    double price;           // Trade price
    double quantity;        // Trade quantity
    int64_t trade_id;       // Trade ID
    uint8_t is_buyer_maker; // 1 if buyer is maker, 0 otherwise
} trade_record_t;

// Convert Unix timestamp to human-readable date
void format_timestamp(int64_t timestamp, char *buffer, size_t size) {
    time_t time_val = timestamp / 1000; // Convert from milliseconds to seconds
    struct tm *tm_info = localtime(&time_val);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <trade_file> [count]\n", argv[0]);
        printf("  trade_file - Path to binary trade file\n");
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
    size_t record_size = sizeof(trade_record_t);
    size_t record_count = file_size / record_size;
    
    printf("File: %s\n", file_path);
    printf("File size: %ld bytes\n", file_size);
    printf("Record size: %zu bytes\n", record_size);
    printf("Total records: %zu\n\n", record_count);
    
    // Display header
    printf("%-24s %-24s %-15s %-15s %-12s %s\n", 
           "Event Time", "Trade Time", "Price", "Quantity", "Trade ID", "Buyer Maker");
    printf("%-24s %-24s %-15s %-15s %-12s %s\n", 
           "------------------------", "------------------------", "---------------", 
           "---------------", "------------", "------------");
    
    // Read and display records
    int count = 0;
    trade_record_t record;
    char event_time_str[32], trade_time_str[32];
    
    while (fread(&record, record_size, 1, file) == 1) {
        // Check if we've reached the maximum count
        if (max_count > 0 && count >= max_count) {
            break;
        }
        
        // Format timestamps
        format_timestamp(record.event_time, event_time_str, sizeof(event_time_str));
        format_timestamp(record.trade_time, trade_time_str, sizeof(trade_time_str));
        
        // Display record
        printf("%-24s %-24s %-15.8f %-15.8f %-12lld %s\n", 
               event_time_str, trade_time_str, record.price, record.quantity, 
               (long long)record.trade_id, record.is_buyer_maker ? "Yes" : "No");
        
        count++;
    }
    
    printf("\nDisplayed %d out of %zu records\n", count, record_count);
    
    fclose(file);
    return 0;
}