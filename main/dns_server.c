#include "dns_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "dns_server";
static int dns_sock = -1;
static TaskHandle_t dns_task_handle = NULL;

#define DNS_PORT 53
#define DNS_MAX_PACKET_SIZE 512

typedef struct __attribute__((packed))
{
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

static void dns_server_task(void *pvParameters)
{
    uint8_t rx_buffer[DNS_MAX_PACKET_SIZE];
    struct sockaddr_in server_addr;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DNS_PORT);

    dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (dns_sock < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    if (bind(dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(dns_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);

    while (1)
    {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(dns_sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);

        if (len < 0)
        {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }

        if (len < sizeof(dns_header_t)) continue;

        dns_header_t *header = (dns_header_t *)rx_buffer;
        
        // Prepare Response
        header->flags = htons(0x8180); // Response, No Error
        header->an_count = header->qd_count; // Number of answers = number of questions

        uint8_t *answer_ptr = rx_buffer + len;
        
        // Simple redirection: Append an A-Record for each question
        // Note: This is a very simplified DNS server for redirection purposes.
        // It assume the packet only has questions.
        
        // For each question, we append an answer pointing to 192.168.4.1
        // We only support one answer for simplicity to keep packet size small
        if (ntohs(header->qd_count) > 0)
        {
            uint8_t *query_ptr = rx_buffer + sizeof(dns_header_t);
            // Skip the name in the question
            while (*query_ptr != 0) query_ptr += (*query_ptr + 1);
            query_ptr += 5; // Skip Null terminator, Type (2 bytes), Class (2 bytes)

            // Answer section
            // Name: Reference to the query name (Offset to the name in query)
            *answer_ptr++ = 0xc0;
            *answer_ptr++ = 0x0c; // 0x0c is the offset of the first name in the packet (just after header)

            // Type A (Host Address)
            *answer_ptr++ = 0x00;
            *answer_ptr++ = 0x01;

            // Class IN
            *answer_ptr++ = 0x00;
            *answer_ptr++ = 0x01;

            // TTL (10 seconds)
            *answer_ptr++ = 0x00;
            *answer_ptr++ = 0x00;
            *answer_ptr++ = 0x00;
            *answer_ptr++ = 0x0a;

            // Data Length (4 bytes for IPv4)
            *answer_ptr++ = 0x00;
            *answer_ptr++ = 0x04;

            // IP Address: 192.168.4.1
            *answer_ptr++ = 192;
            *answer_ptr++ = 168;
            *answer_ptr++ = 4;
            *answer_ptr++ = 1;

            int response_len = answer_ptr - rx_buffer;
            sendto(dns_sock, rx_buffer, response_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        }
    }

    if (dns_sock != -1)
    {
        close(dns_sock);
        dns_sock = -1;
    }
    vTaskDelete(NULL);
}

void dns_server_start(void)
{
    if (dns_task_handle != NULL) return;
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle);
}

void dns_server_stop(void)
{
    if (dns_task_handle == NULL) return;
    vTaskDelete(dns_task_handle);
    dns_task_handle = NULL;
    if (dns_sock != -1)
    {
        close(dns_sock);
        dns_sock = -1;
    }
}
