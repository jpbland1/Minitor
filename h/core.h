#ifndef MINITOR_CORE_H
#define MINITOR_CORE_H

#include "./consensus.h"
#include "./circuit.h"
#include "./onion_service.h"

void v_send_init_circuit( int length, CircuitStatus target_status, OnionService* service, int desc_index, int target_relay_index, OnionRelay* start_relay, OnionRelay* end_relay, HsCrypto* hs_crypto );
void v_minitor_daemon( void* pv_parameters );
void v_set_hsdir_timer( MinitorTimer hsdir_timer );
int d_get_standby_count();

extern MinitorTimer keepalive_timer;
extern MinitorTimer timeout_timer;
extern OnionCircuit* onion_circuits;
extern OnionService* onion_services;
extern MinitorQueue core_task_queue;
extern MinitorMutex circuits_mutex;

#endif
