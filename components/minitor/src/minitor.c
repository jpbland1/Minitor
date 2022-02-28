#include <stddef.h>
#include <stdlib.h>

#include "esp_log.h"

#include "../include/config.h"
#include "../include/minitor.h"
#include "../h/models/db.h"
#include "../h/consensus.h"
#include "../h/circuit.h"
#include "../h/onion_service.h"

WOLFSSL_CTX* xMinitorWolfSSL_Context;

static void v_keep_circuitlist_alive( DoublyLinkedOnionCircuitList* list ) {
  int i;
  Cell padding_cell;
  DoublyLinkedOnionCircuit* node;
  unsigned char* packed_cell;

  padding_cell.command = PADDING;
  padding_cell.payload = NULL;
  node = list->head;

  for ( i = 0; i < list->length; i++ ) {
    padding_cell.circ_id = node->circuit.circ_id;
    packed_cell = pack_and_free( &padding_cell );

    if ( wolfSSL_send( node->circuit.ssl, packed_cell, CELL_LEN, 0 ) < 0 ) {
#ifdef DEBUG_MINITOR
      ESP_LOGE( MINITOR_TAG, "Failed to send padding cell on circ_id: %d", node->circuit.circ_id );
#endif
    }

    free( packed_cell );
    node = node->next;
  }
}

static void v_circuit_keepalive( void* pv_parameters ) {
  time_t now;
  unsigned long int fresh_until;

  while ( 1 ) {
    vTaskDelay( 1000 * 60 / portTICK_PERIOD_MS );

    xSemaphoreTake( standby_circuits_mutex, portMAX_DELAY );

    v_keep_circuitlist_alive( &standby_circuits );

    xSemaphoreGive( standby_circuits_mutex );

    xSemaphoreTake( standby_rend_circuits_mutex, portMAX_DELAY );

    v_keep_circuitlist_alive( &standby_rend_circuits );

    xSemaphoreGive( standby_rend_circuits_mutex );

/*
    xSemaphoreTake( network_consensus_mutex, portMAX_DELAY );

    fresh_until = network_consensus.fresh_until;

    xSemaphoreGive( network_consensus_mutex );

    time( &now );

    if ( now >= fresh_until )
    {
      while ( d_fetch_consensus_info() < 0 )
      {
#ifdef DEBUG_MINITOR
        ESP_LOGE( MINITOR_TAG, "couldn't refresh the consensus, retrying" );
#endif

        vTaskDelay( 1000 * 5 / portTICK_PERIOD_MS );
      }
    }
*/
  }
}

// intialize tor
int v_minitor_INIT()
{
  circ_id_mutex = xSemaphoreCreateMutex();
  network_consensus_mutex = xSemaphoreCreateMutex();
  suitable_relays_mutex = xSemaphoreCreateMutex();
  used_guards_mutex = xSemaphoreCreateMutex();
  hsdir_relays_mutex = xSemaphoreCreateMutex();
  standby_circuits_mutex = xSemaphoreCreateMutex();
  standby_rend_circuits_mutex = xSemaphoreCreateMutex();

  wolfSSL_Init();
  /* wolfSSL_Debugging_ON(); */

  if ( d_initialize_database() < 0 )
  {
#ifdef DEBUG_MINITOR
    ESP_LOGE( MINITOR_TAG, "couldn't setup minitor sqlite3 database" );
#endif

    return -1;
  }

  if ( ( xMinitorWolfSSL_Context = wolfSSL_CTX_new( wolfTLSv1_2_client_method() ) ) == NULL )
  {
#ifdef DEBUG_MINITOR
    ESP_LOGE( MINITOR_TAG, "couldn't setup wolfssl context" );
#endif

    return -1;
  }

  // fetch network consensus
  if ( d_fetch_consensus_info() < 0 )
  {
    return -1;
  }

  xTaskCreatePinnedToCore(
    v_circuit_keepalive,
    "CIRCUIT_KEEPALIVE",
    8192,
    NULL,
    6,
    NULL,
    tskNO_AFFINITY
  );

  return 1;
}

// ONION SERVICES
OnionService* px_setup_hidden_service( unsigned short local_port, unsigned short exit_port, const char* onion_service_directory )
{
  int i;
  DoublyLinkedOnionCircuit* node;
  OnionService* onion_service = malloc( sizeof( OnionService ) );

  memset( onion_service, 0, sizeof( OnionService ) );

  onion_service->local_port = local_port;
  onion_service->exit_port = exit_port;
  onion_service->rx_queue = xQueueCreate( 5, sizeof( OnionMessage* ) );

  if ( d_generate_hs_keys( onion_service, onion_service_directory ) < 0 )
  {
#ifdef DEBUG_MINITOR
    ESP_LOGE( MINITOR_TAG, "Failed to generate hs keys" );
#endif

    return NULL;
  }

  // setup starting circuits
  if ( d_setup_init_circuits( 3 ) < 3 )
  {
#ifdef DEBUG_MINITOR
    ESP_LOGE( MINITOR_TAG, "Failed to setup init circuits" );
#endif

    return NULL;
  }

  // take two circuits from the standby circuits list
  // BEGIN mutex
  xSemaphoreTake( standby_circuits_mutex, portMAX_DELAY );

  if ( standby_circuits.length < 3 )
  {
#ifdef DEBUG_MINITOR
    ESP_LOGE( MINITOR_TAG, "Not enough standby circuits to register intro points" );
#endif

    xSemaphoreGive( standby_circuits_mutex );
    // END mutex

    return NULL;
  }

  // set the onion services head to the standby circuit head
  onion_service->intro_circuits.head = standby_circuits.head;
  // set the onion services tail to the second standby circuit
  onion_service->intro_circuits.tail = standby_circuits.head->next->next;

  // if there is a fourth standby circuit, set its previous to NULL
  if ( standby_circuits.length > 3 )
  {
    standby_circuits.head->next->next->next->previous = NULL;
  }

  // set the standby circuit head to the thrid, possibly NULL
  standby_circuits.head = standby_circuits.head->next->next->next;
  // disconnect our tail from the other standby circuits
  onion_service->intro_circuits.tail->next = NULL;
  // set our intro length to three
  onion_service->intro_circuits.length = 3;
  // subtract three from the standby_circuits length
  standby_circuits.length -= 3;

  xSemaphoreGive( standby_circuits_mutex );
  // END mutex

  // send establish intro commands to our three circuits
  node = onion_service->intro_circuits.head;

  for ( i = 0; i < onion_service->intro_circuits.length; i++ )
  {
    node->circuit.rx_queue = onion_service->rx_queue;

    if ( d_router_establish_intro( &node->circuit ) < 0 ) {
#ifdef DEBUG_MINITOR
      ESP_LOGE( MINITOR_TAG, "Failed to establish intro with a circuit" );
#endif

      return NULL;
    }

    node->circuit.status = CIRCUIT_INTRO_POINT;

    node = node->next;
  }

  if ( d_push_hsdir( onion_service ) < 0 )
  {
#ifdef DEBUG_MINITOR
    ESP_LOGE( MINITOR_TAG, "Failed to push hsdir" );
#endif

    return NULL;
  }

  if ( d_setup_init_rend_circuits( 2 ) < 2 )
  {
#ifdef DEBUG_MINITOR
    ESP_LOGE( MINITOR_TAG, "Failed to setup init rend circuits" );
#endif

    return NULL;
  }

  ESP_LOGE( MINITOR_TAG, "starting HANDLE_HS" );

  // create a task to block on the rx_queue
  xTaskCreatePinnedToCore(
    v_handle_onion_service,
    "HANDLE_HS",
    8192,
    (void*)(onion_service),
    6,
    NULL,
    tskNO_AFFINITY
  );

  // return the onion service
  return onion_service;
}
