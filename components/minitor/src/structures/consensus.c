#include <stddef.h>

#include "../../h/structures/consensus.h"

// shared state must be protected by mutex
NetworkConsensus network_consensus = {
  .method = 0,
  .valid_after = 0,
  .fresh_until = 0,
  .valid_until = 0,
  .hsdir_interval = HSDIR_INTERVAL_DEFAULT,
  .hsdir_n_replicas = HSDIR_N_REPLICAS_DEFAULT,
  .hsdir_spread_store = HSDIR_SPREAD_STORE_DEFAULT,
};
SemaphoreHandle_t network_consensus_mutex;

DoublyLinkedOnionRelayList suitable_relays = {
  .length = 0,
  .head = NULL,
  .tail = NULL,
};
SemaphoreHandle_t suitable_relays_mutex;

DoublyLinkedOnionRelayList hsdir_relays = {
  .length = 0,
  .head = NULL,
  .tail = NULL,
};
SemaphoreHandle_t hsdir_relays_mutex;

// add a linked onion relay to a doubly linked list of onion relays
void v_add_relay_to_list( DoublyLinkedOnionRelay* node, DoublyLinkedOnionRelayList* list ) {
  // if our length is 0, just set this node as the head and tail
  if ( list->length == 0 ) {
    list->head = node;
    list->tail = node;
  // otherwise set the new node's previous to the current tail, set the current tail's
  // next to the new node and set the new node as the new tail
  } else {
    node->previous = list->tail;
    list->tail->next = node;
    list->tail = node;
  }

  // increase the length of the list
  list->length++;
}