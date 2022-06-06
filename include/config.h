#ifndef MINITOR_CONFIG_H
#define MINITOR_CONFIG_H

#define DEBUG_MINITOR
#define MINITOR_RELAY_MAX 60
#define MINITOR_DIR_ADDR 0x76a40dcc
#define MINITOR_DIR_ADDR_STR "204.13.164.118"
#define MINITOR_DIR_PORT 80
//#define MINITOR_CHUTNEY
#define MINITOR_CHUTNEY_ADDRESS 0x7602a8c0
#define MINITOR_CHUTNEY_ADDRESS_STR "192.168.2.118"
#define MINITOR_CHUTNEY_DIR_PORT 7000
#define FILESYSTEM_PREFIX "/sdcard/"

extern const char* tor_authorities[];
extern int tor_authorities_count;

#endif
