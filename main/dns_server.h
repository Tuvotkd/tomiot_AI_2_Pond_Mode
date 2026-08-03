#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include <stdbool.h>

/**
 * @brief Start the captive portal DNS server
 * 
 * This server will respond to ALL DNS queries with the specified IP address.
 * Standard ESP32 SoftAP IP is "192.168.4.1".
 */
void dns_server_start(void);

/**
 * @brief Stop the DNS server
 */
void dns_server_stop(void);

#endif // DNS_SERVER_H
