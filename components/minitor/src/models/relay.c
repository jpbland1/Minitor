#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "driver/spi_master.h"

#include "../../include/config.h"
#include "../../h/constants.h"
#include "../../h/consensus.h"
#include "../../h/models/issi.h"
#include "../../h/models/relay.h"

uint32_t hsdir_relay_count = 0;
uint32_t random_offset_count = 1;
int current_root_addr = HSDIR_TREE_ROOT;
int previous_root_addr = HSDIR_TREE_ROOT;

static int d_get_relay_at_address( BinaryRelay* b_relay, int addr )
{
  int err;

  err = spi_device_acquire_bus( issi_spi, portMAX_DELAY );

  if ( err != 0 ) {
    ESP_LOGE( MINITOR_TAG, "Failed to aquire the spi bus" );

    return -1;
  }

  spi_transaction_ext_t t;

  t.base.cmd = ISSI_READ;
  t.base.addr = ( addr & 0xffffff );
  t.base.length = 0;
  t.base.tx_buffer = NULL;
  t.base.rxlength = 8 * sizeof( BinaryRelay );
  t.base.rx_buffer = b_relay;
  t.base.flags = SPI_TRANS_VARIABLE_DUMMY;
  t.dummy_bits = 9;

  err = spi_device_polling_transmit( issi_spi, &t );

  spi_device_release_bus( issi_spi );

  if ( err != 0 ) {
    ESP_LOGE( MINITOR_TAG, "Failed to transmit the spi transaction" );

    return -1;
  }

  // ensure dma memory is stable, sort of a hack
  vTaskDelay( 1 );

  return err;
}

static int d_store_relay_at_address( BinaryRelay* b_relay, int addr )
{
  int err;

  err = spi_device_acquire_bus( issi_spi, portMAX_DELAY );

  if ( err != 0 ) {
    ESP_LOGE( MINITOR_TAG, "Failed to aquire the spi bus" );

    return -1;
  }

  spi_transaction_ext_t t;

  t.base.cmd = ISSI_WRITE;
  t.base.addr = ( addr & 0xffffff );
  t.base.length = 8 * sizeof( BinaryRelay );
  t.base.tx_buffer = b_relay;
  t.base.rxlength = 0;
  t.base.rx_buffer = NULL;
  t.base.flags = 0;

  err = spi_device_polling_transmit( issi_spi, &t );

  spi_device_release_bus( issi_spi );

  if ( err != 0 ) {
    ESP_LOGE( MINITOR_TAG, "Failed to transmit the spi transaction" );

    return -1;
  }

  // ensure dma memory is stable, sort of a hack
  vTaskDelay( 1 );

  return err;
}

int d_reset_hsdir_relay_tree()
{
  BinaryRelay* b_relay;

  current_root_addr = HSDIR_TREE_ROOT;
  previous_root_addr = HSDIR_TREE_ROOT;

  b_relay = heap_caps_malloc( sizeof( BinaryRelay ), MALLOC_CAP_DMA );

  memset( b_relay, 0xff, sizeof( BinaryRelay ) );

  if ( d_store_relay_at_address( b_relay, current_root_addr ) != 0 )
  {
    return -1;
  }

  hsdir_relay_count = 0;

  free( b_relay );

  return 0;
}

static int d_rebalance_relays( int parent_addr, int child_addr, int current )
{
  int ret = 0;
  int tmp_addr;
  int root_addr;
  BinaryRelay* parent_relay;
  BinaryRelay* child_relay;
  BinaryRelay* tmp_relay;

  parent_relay = heap_caps_malloc( sizeof( BinaryRelay ), MALLOC_CAP_DMA );
  child_relay = heap_caps_malloc( sizeof( BinaryRelay ), MALLOC_CAP_DMA );
  tmp_relay = heap_caps_malloc( sizeof( BinaryRelay ), MALLOC_CAP_DMA );

  if ( current == 1 )
  {
    root_addr = current_root_addr;
  }
  else
  {
    root_addr = previous_root_addr;
  }

  while ( child_addr != root_addr )
  {
    if ( d_get_relay_at_address( parent_relay, parent_addr ) != 0 )
    {
      ret = -1;
      goto finish;
    }

    if ( parent_relay->avl_blocks[current].right_addr == child_addr )
    {
      parent_relay->avl_blocks[current].balance++;

      if ( parent_relay->avl_blocks[current].balance > 1 )
      {
        if ( d_get_relay_at_address( child_relay, child_addr ) != 0 )
        {
          ret = -1;
          goto finish;
        }

        // rotate left
        if ( child_relay->avl_blocks[current].balance > 0 )
        {
          if ( parent_addr == root_addr )
          {
            if ( current == 1 )
            {
              current_root_addr = child_addr;
            }
            else
            {
              previous_root_addr = child_addr;
            }
          }
          else
          {
            if ( d_get_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].parent_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            if ( tmp_relay->avl_blocks[current].right_addr == parent_addr )
            {
              tmp_relay->avl_blocks[current].right_addr = child_addr;
            }
            else
            {
              tmp_relay->avl_blocks[current].left_addr = child_addr;
            }

            if ( d_store_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].parent_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }

          child_relay->avl_blocks[current].parent_addr = parent_relay->avl_blocks[current].parent_addr;
          parent_relay->avl_blocks[current].parent_addr = child_addr;
          parent_relay->avl_blocks[current].right_addr = child_relay->avl_blocks[current].left_addr;
          child_relay->avl_blocks[current].left_addr = parent_addr;

          if ( parent_relay->avl_blocks[current].right_addr != -1 )
          {
            if ( d_get_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].right_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            tmp_relay->avl_blocks[current].parent_addr = parent_addr;

            if ( d_store_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].right_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }

          parent_relay->avl_blocks[current].balance = 0;
          child_relay->avl_blocks[current].balance = 0;
        }
        // rotate right left
        else if ( child_relay->avl_blocks[current].balance < 0 )
        {
          tmp_addr = child_relay->avl_blocks[current].left_addr;

          if ( parent_addr == root_addr )
          {
            if ( current == 1 )
            {
              current_root_addr = tmp_addr;
            }
            else
            {
              previous_root_addr = tmp_addr;
            }
          }
          else
          {
            if ( d_get_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].parent_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            if ( tmp_relay->avl_blocks[current].right_addr == parent_addr )
            {
              tmp_relay->avl_blocks[current].right_addr = tmp_addr;
            }
            else
            {
              tmp_relay->avl_blocks[current].left_addr = tmp_addr;
            }

            if ( d_store_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].parent_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }

          if ( d_get_relay_at_address( tmp_relay, tmp_addr ) != 0 )
          {
            ret = -1;
            goto finish;
          }

          tmp_relay->avl_blocks[current].parent_addr = parent_relay->avl_blocks[current].parent_addr;
          parent_relay->avl_blocks[current].right_addr = tmp_relay->avl_blocks[current].left_addr;
          tmp_relay->avl_blocks[current].left_addr = parent_addr;
          child_relay->avl_blocks[current].left_addr = tmp_relay->avl_blocks[current].right_addr;
          tmp_relay->avl_blocks[current].right_addr = child_addr;
          child_relay->avl_blocks[current].parent_addr = tmp_addr;
          parent_relay->avl_blocks[current].parent_addr = tmp_addr;

          if ( tmp_relay->avl_blocks[current].balance > 0 )
          {
            parent_relay->avl_blocks[current].balance = -1;
            child_relay->avl_blocks[current].balance = 0;
          }
          else if ( tmp_relay->avl_blocks[current].balance < 0 )
          {
            parent_relay->avl_blocks[current].balance = 0;
            child_relay->avl_blocks[current].balance = 1;
          }
          else
          {
            parent_relay->avl_blocks[current].balance = 0;
            child_relay->avl_blocks[current].balance = 0;
          }

          tmp_relay->avl_blocks[current].balance = 0;

          if ( d_store_relay_at_address( tmp_relay, tmp_addr ) != 0 )
          {
            ret = -1;
            goto finish;
          }

          if ( parent_relay->avl_blocks[current].right_addr != -1 )
          {
            if ( d_get_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].right_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            tmp_relay->avl_blocks[current].parent_addr = parent_addr;

            if ( d_store_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].right_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }

          if ( child_relay->avl_blocks[current].left_addr != -1 )
          {
            if ( d_get_relay_at_address( tmp_relay, child_relay->avl_blocks[current].left_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            tmp_relay->avl_blocks[current].parent_addr = child_addr;

            if ( d_store_relay_at_address( tmp_relay, child_relay->avl_blocks[current].left_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }
        }

        if ( d_store_relay_at_address( child_relay, child_addr ) != 0 )
        {
          ret = -1;
          goto finish;
        }

        if ( d_store_relay_at_address( parent_relay, parent_addr ) != 0 )
        {
          ret = -1;
          goto finish;
        }

        // balance restored, now exit
        break;
      }
    }
    else if ( parent_relay->avl_blocks[current].left_addr == child_addr )
    {
      parent_relay->avl_blocks[current].balance--;

      if ( parent_relay->avl_blocks[current].balance < -1 )
      {
        if ( d_get_relay_at_address( child_relay, child_addr ) != 0 )
        {
          ret = -1;
          goto finish;
        }

        // rotate right
        if ( child_relay->avl_blocks[current].balance < 0 )
        {
          if ( parent_addr == root_addr )
          {
            if ( current == 1 )
            {
              current_root_addr = child_addr;
            }
            else
            {
              previous_root_addr = child_addr;
            }
          }
          else
          {
            if ( d_get_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].parent_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            if ( tmp_relay->avl_blocks[current].right_addr == parent_addr )
            {
              tmp_relay->avl_blocks[current].right_addr = child_addr;
            }
            else
            {
              tmp_relay->avl_blocks[current].left_addr = child_addr;
            }

            if ( d_store_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].parent_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }

          child_relay->avl_blocks[current].parent_addr = parent_relay->avl_blocks[current].parent_addr;
          parent_relay->avl_blocks[current].parent_addr = child_addr;
          parent_relay->avl_blocks[current].left_addr = child_relay->avl_blocks[current].right_addr;
          child_relay->avl_blocks[current].right_addr = parent_addr;

          if ( parent_relay->avl_blocks[current].left_addr != -1 )
          {
            if ( d_get_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].left_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            tmp_relay->avl_blocks[current].parent_addr = parent_addr;

            if ( d_store_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].left_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }

          parent_relay->avl_blocks[current].balance = 0;
          child_relay->avl_blocks[current].balance = 0;
        }
        // rotate left right
        else if ( child_relay->avl_blocks[current].balance > 0 )
        {
          tmp_addr = child_relay->avl_blocks[current].right_addr;

          if ( parent_addr == root_addr )
          {
            if ( current == 1 )
            {
              current_root_addr = tmp_addr;
            }
            else
            {
              previous_root_addr = tmp_addr;
            }
          }
          else
          {
            if ( d_get_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].parent_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            if ( tmp_relay->avl_blocks[current].right_addr == parent_addr )
            {
              tmp_relay->avl_blocks[current].right_addr = tmp_addr;
            }
            else
            {
              tmp_relay->avl_blocks[current].left_addr = tmp_addr;
            }

            if ( d_store_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].parent_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }

          if ( d_get_relay_at_address( tmp_relay, tmp_addr ) != 0 )
          {
            ret = -1;
            goto finish;
          }

          tmp_relay->avl_blocks[current].parent_addr = parent_relay->avl_blocks[current].parent_addr;
          parent_relay->avl_blocks[current].left_addr = tmp_relay->avl_blocks[current].right_addr;
          tmp_relay->avl_blocks[current].right_addr = parent_addr;
          child_relay->avl_blocks[current].right_addr = tmp_relay->avl_blocks[current].left_addr;
          tmp_relay->avl_blocks[current].left_addr = child_addr;
          child_relay->avl_blocks[current].parent_addr = tmp_addr;
          parent_relay->avl_blocks[current].parent_addr = tmp_addr;

          if ( tmp_relay->avl_blocks[current].balance > 0 )
          {
            child_relay->avl_blocks[current].balance = -1;
            parent_relay->avl_blocks[current].balance = 0;
          }
          else if ( tmp_relay->avl_blocks[current].balance < 0 )
          {
            parent_relay->avl_blocks[current].balance = 1;
            child_relay->avl_blocks[current].balance = 0;
          }
          else
          {
            parent_relay->avl_blocks[current].balance = 0;
            child_relay->avl_blocks[current].balance = 0;
          }

          tmp_relay->avl_blocks[current].balance = 0;

          if ( d_store_relay_at_address( tmp_relay, tmp_addr ) != 0 )
          {
            ret = -1;
            goto finish;
          }

          if ( parent_relay->avl_blocks[current].left_addr != -1 )
          {
            if ( d_get_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].left_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            tmp_relay->avl_blocks[current].parent_addr = parent_addr;

            if ( d_store_relay_at_address( tmp_relay, parent_relay->avl_blocks[current].left_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }

          if ( child_relay->avl_blocks[current].right_addr != -1 )
          {
            if ( d_get_relay_at_address( tmp_relay, child_relay->avl_blocks[current].right_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }

            tmp_relay->avl_blocks[current].parent_addr = child_addr;

            if ( d_store_relay_at_address( tmp_relay, child_relay->avl_blocks[current].right_addr ) != 0 )
            {
              ret = -1;
              goto finish;
            }
          }
        }

        if ( d_store_relay_at_address( child_relay, child_addr ) != 0 )
        {
          ret = -1;
          goto finish;
        }

        if ( d_store_relay_at_address( parent_relay, parent_addr ) != 0 )
        {
          ret = -1;
          goto finish;
        }

        // balance restored, now exit
        break;
      }
    }

    if ( d_store_relay_at_address( parent_relay, parent_addr ) != 0 )
    {
      ret = -1;
      goto finish;
    }

    // our addition balanced the subtree, no need to continue up
    if ( parent_relay->avl_blocks[current].balance == 0 )
    {
      break;
    }

    child_addr = parent_addr;
    parent_addr = parent_relay->avl_blocks[current].parent_addr;
  }

finish:
  free( parent_relay );
  free( child_relay );
  free( tmp_relay );

  return ret;
}

static int d_create_hsdir_relay_by_srv( OnionRelay* onion_relay, int current, int write_addr )
{
  int ret;
  int next_addr;
  int root_addr;
  BinaryRelay* b_relay;

  b_relay = heap_caps_malloc( sizeof( BinaryRelay ), MALLOC_CAP_DMA );

  if ( current == 1 )
  {
    root_addr = current_root_addr;
  }
  else
  {
    root_addr = previous_root_addr;
  }

  next_addr = root_addr;

  while ( 1 )
  {
    if ( d_get_relay_at_address( b_relay, next_addr ) != 0 )
    {
      goto fail;
    }

    if ( next_addr == root_addr )
    {
      if ( b_relay->avl_blocks[current].parent_addr == 0xffffffff )
      {
        b_relay->avl_blocks[current].parent_addr = root_addr + sizeof( BinaryRelay );
        b_relay->avl_blocks[current].left_addr = -1;
        b_relay->avl_blocks[current].right_addr = -1;
        b_relay->avl_blocks[current].balance = 0;

        memcpy( &b_relay->relay, onion_relay, sizeof( OnionRelay ) );

        if ( d_store_relay_at_address( b_relay, root_addr ) != 0 )
        {
          goto fail;
        }

        if ( current == 1 )
        {
          hsdir_relay_count++;
        }

        free( b_relay );

        return root_addr;
      }

      if ( current == 1 )
      {
        write_addr = b_relay->avl_blocks[current].parent_addr;
      }
    }

    if ( current == 1 )
    {
      ret = memcmp( onion_relay->id_hash, b_relay->relay.id_hash, H_LENGTH );
    }
    else
    {
      ret = memcmp( onion_relay->id_hash_previous, b_relay->relay.id_hash_previous, H_LENGTH );
    }

    if ( ret < 0 )
    {
      if ( b_relay->avl_blocks[current].left_addr == -1 )
      {
        b_relay->avl_blocks[current].left_addr = write_addr;

        break;
      }
      else
      {
        next_addr = b_relay->avl_blocks[current].left_addr;
      }
    }
    else
    {
      if ( b_relay->avl_blocks[current].right_addr == -1 )
      {
        b_relay->avl_blocks[current].right_addr = write_addr;

        break;
      }
      else
      {
        next_addr = b_relay->avl_blocks[current].right_addr;
      }
    }
  }

  if ( d_store_relay_at_address( b_relay, next_addr ) != 0 )
  {
    goto fail;
  }

  if ( current == 1 )
  {
    memset( b_relay, 0xff, sizeof( BinaryRelay ) );

    memcpy( &b_relay->relay, onion_relay, sizeof( OnionRelay ) );
  }
  else
  {
    if ( d_get_relay_at_address( b_relay, write_addr ) != 0 )
    {
      goto fail;
    }
  }

  b_relay->avl_blocks[current].parent_addr = next_addr;
  b_relay->avl_blocks[current].left_addr = -1;
  b_relay->avl_blocks[current].right_addr = -1;
  b_relay->avl_blocks[current].balance = 0;

  if ( d_store_relay_at_address( b_relay, write_addr ) != 0 )
  {
    goto fail;
  }

  if ( current == 1 )
  {
    if ( d_get_relay_at_address( b_relay, root_addr ) != 0 )
    {
      goto fail;
    }

    b_relay->avl_blocks[current].parent_addr = write_addr + sizeof( BinaryRelay );

    if ( d_store_relay_at_address( b_relay, root_addr ) != 0 )
    {
      goto fail;
    }
  }

  if ( d_rebalance_relays( next_addr, write_addr, current ) != 0 )
  {
    goto fail;
  }

  if ( current == 1 )
  {
    hsdir_relay_count++;
  }

  free( b_relay );

  return write_addr;

fail:
  free( b_relay );

  return -1;
}

int d_create_hsdir_relay( OnionRelay* onion_relay )
{
  int write_addr;

  write_addr = d_create_hsdir_relay_by_srv( onion_relay, 1, -1 );

  if ( write_addr < 0 )
  {
    return -1;
  }

  return d_create_hsdir_relay_by_srv( onion_relay, 0, write_addr );
}

int d_traverse_hsdir_relays_in_order( BinaryRelay* b_relay, int next_addr, int* previous_addr, int offset, int current )
{
  int i;
  int root_addr;

  if ( current == 1 )
  {
    root_addr = current_root_addr;
  }
  else
  {
    root_addr = previous_root_addr;
  }

  // traverse to the next node offset times
  for ( i = 0; i < offset; i++ )
  {
    if ( hsdir_relay_count == 1 )
    {
      if ( d_get_relay_at_address( b_relay, root_addr ) != 0 )
      {
        return -1;
      }

      return 0;
    }

    if ( d_get_relay_at_address( b_relay, next_addr ) != 0 )
    {
      return -1;
    }

    if ( next_addr == root_addr && b_relay->avl_blocks[current].parent_addr == 0xffffffff )
    {
      return -1;
    }

    // if we moved up and left, don't count it as next
    if ( *previous_addr == b_relay->avl_blocks[current].right_addr )
    {
      i--;
      *previous_addr = next_addr;

      // at root, start traversing down to first node
      if ( next_addr == root_addr )
      {
        next_addr = b_relay->avl_blocks[current].left_addr;
      }
      else
      {
        next_addr = b_relay->avl_blocks[current].parent_addr;
      }
    }
    else if ( *previous_addr == b_relay->avl_blocks[current].left_addr )
    {
      *previous_addr = next_addr;

      if ( b_relay->avl_blocks[current].right_addr == -1 )
      {
        next_addr = b_relay->avl_blocks[current].parent_addr;
      }
      else
      {
        next_addr = b_relay->avl_blocks[current].right_addr;
      }
    }
    // if we traversed down or we just started at the root node
    else if ( *previous_addr == b_relay->avl_blocks[current].parent_addr || ( *previous_addr == root_addr && next_addr == root_addr ) )
    {
      *previous_addr = next_addr;

      if ( b_relay->avl_blocks[current].left_addr == -1 )
      {
        if ( b_relay->avl_blocks[current].right_addr == -1 )
        {
          next_addr = b_relay->avl_blocks[current].parent_addr;
        }
        else
        {
          next_addr = b_relay->avl_blocks[current].right_addr;
        }
      }
      else
      {
        i--;
        next_addr = b_relay->avl_blocks[current].left_addr;
      }
    }
  }

  return next_addr;
}

OnionRelay* px_get_hsdir_relay_by_id_hash( uint8_t* id_hash, uint8_t* identity, int offset, DoublyLinkedOnionRelayList* used_relays, int current )
{
  int i;
  int ret;
  int next_addr;
  int previous_addr;
  int root_addr;
  BinaryRelay* b_relay;
  DoublyLinkedOnionRelay* used_relay;
  OnionRelay* onion_relay;

  if ( current == 1 )
  {
    root_addr = current_root_addr;
  }
  else
  {
    root_addr = previous_root_addr;
  }

  b_relay = heap_caps_malloc( sizeof( BinaryRelay ), MALLOC_CAP_DMA );

  next_addr = root_addr;

  while ( 1 )
  {
    if ( d_get_relay_at_address( b_relay, next_addr ) != 0 )
    {
      goto fail;
    }

    if ( next_addr == root_addr )
    {
      if ( b_relay->avl_blocks[current].parent_addr == 0xffffffff )
      {
        goto fail;
      }
    }

    if ( current == 1 )
    {
      ret = memcmp( id_hash, b_relay->relay.id_hash, H_LENGTH );
    }
    else
    {
      ret = memcmp( id_hash, b_relay->relay.id_hash_previous, H_LENGTH );
    }

    if ( ret < 0 )
    {
      if ( b_relay->avl_blocks[current].left_addr == -1 )
      {
        if ( identity != NULL )
        {
          goto fail;
        }

        previous_addr = next_addr;

        if ( b_relay->avl_blocks[current].right_addr != -1 )
        {
          next_addr = b_relay->avl_blocks[current].right_addr;
        }
        else
        {
          next_addr = b_relay->avl_blocks[current].parent_addr;
        }

        break;
      }
      else
      {
        next_addr = b_relay->avl_blocks[current].left_addr;
      }
    }
    else if ( ret > 0 )
    {
      // we have stopped at a node that is less than us
      // we need to traverse to the next node in order
      // since that will be larger than us or will be the
      // first node
      if ( b_relay->avl_blocks[current].right_addr == -1 )
      {
        if ( identity != NULL )
        {
          goto fail;
        }

        offset++;
        previous_addr = next_addr;
        next_addr = b_relay->avl_blocks[current].parent_addr;

        break;
      }
      else
      {
        next_addr = b_relay->avl_blocks[current].right_addr;
      }
    }
    else
    {
      if ( identity == NULL || memcmp( identity, b_relay->relay.identity, ID_LENGTH ) == 0 )
      {
        previous_addr = next_addr;

        if ( b_relay->avl_blocks[current].right_addr == -1 )
        {
          next_addr = b_relay->avl_blocks[current].parent_addr;
        }
        else
        {
          next_addr = b_relay->avl_blocks[current].right_addr;
        }

        break;
      }
      else
      {
        if ( b_relay->avl_blocks[current].right_addr == -1 )
        {
          goto fail;
        }
        else
        {
          next_addr = b_relay->avl_blocks[current].right_addr;
        }
      }
    }
  }

  if ( offset == 0 && used_relays != NULL )
  {
    used_relay = used_relays->head;

    for ( i = 0; i < used_relays->length; i++ )
    {
      if ( memcmp( b_relay->relay.identity, used_relay->relay->identity, ID_LENGTH ) == 0 )
      {
        offset = 1;

        break;
      }

      used_relay = used_relay->next;
    }
  }

  while ( offset != 0 )
  {
    next_addr = d_traverse_hsdir_relays_in_order( b_relay, next_addr, &previous_addr, offset, current );

    if ( next_addr < 0 )
    {
      goto fail;
    }

    offset = 0;

    used_relay = used_relays->head;

    for ( i = 0; i < used_relays->length; i++ )
    {
      if ( memcmp( b_relay->relay.identity, used_relay->relay->identity, ID_LENGTH ) == 0 )
      {
        offset = 1;

        break;
      }

      used_relay = used_relay->next;
    }
  }

  onion_relay = malloc( sizeof( OnionRelay ) );
  memcpy( onion_relay, &b_relay->relay, sizeof( OnionRelay ) );
  free( b_relay );

  return onion_relay;

fail:
  free( b_relay );

  return NULL;
}

OnionRelay* px_get_random_hsdir_relay( int want_guard, DoublyLinkedOnionRelayList* relay_list, uint8_t* exclude )
{
  int i;
  int start_addr;
  int previous_addr;
  int offset;
  BinaryRelay* b_relay;
  OnionRelay* onion_relay;
  DoublyLinkedOnionRelay* db_onion_relay;

  b_relay = heap_caps_malloc( sizeof( BinaryRelay ), MALLOC_CAP_DMA );

  start_addr = current_root_addr;
  previous_addr = current_root_addr;
  //offset = ( esp_random() % hsdir_relay_count ) + 1;
  offset = random_offset_count;
  random_offset_count++;

  if ( random_offset_count > hsdir_relay_count )
  {
    ESP_LOGE( MINITOR_TAG, "reset random_offset_count: %d, %d", random_offset_count, hsdir_relay_count );
    random_offset_count = 1;
  }

  do
  {
    start_addr = d_traverse_hsdir_relays_in_order( b_relay, start_addr, &previous_addr, offset, 1 );

    if ( start_addr < 0 )
    {
      goto fail;
    }

    offset = 0;

    if ( relay_list != NULL )
    {
      db_onion_relay = relay_list->head;

      for ( i = 0; i < relay_list->length; i++ )
      {
        if ( memcmp( db_onion_relay->relay->identity, b_relay->relay.identity, ID_LENGTH ) == 0 )
        {
          break;
        }

        db_onion_relay = db_onion_relay->next;
      }

      if ( i != relay_list->length )
      {
        offset = 1;
      }
    }

    if ( want_guard == 1 && ( b_relay->relay.can_guard == 0 || b_relay->relay.is_guard == 1 ) )
    {
      offset = 1;
    }

    if ( exclude != NULL && memcmp( exclude, b_relay->relay.identity, ID_LENGTH ) == 0 )
    {
      offset = 1;
    }
  } while ( offset != 0 );

  onion_relay = malloc( sizeof( OnionRelay ) );
  memcpy( onion_relay, &b_relay->relay, sizeof( OnionRelay ) );
  free( b_relay );

  return onion_relay;

fail:
  free( b_relay );

  return NULL;
}

int d_get_hsdir_count()
{
  return hsdir_relay_count;
}

static int d_update_relay_guard( uint8_t* identity, uint8_t* id_hash, int guard )
{
  int ret;
  int next_addr;
  BinaryRelay* b_relay;

  b_relay = heap_caps_malloc( sizeof( BinaryRelay ), MALLOC_CAP_DMA );

  next_addr = current_root_addr;

  while ( 1 )
  {
    if ( d_get_relay_at_address( b_relay, next_addr ) != 0 )
    {
      goto fail;
    }

    if ( next_addr == current_root_addr )
    {
      if ( b_relay->avl_blocks[1].parent_addr == 0xffffffff )
      {
        goto fail;
      }
    }

    ret = memcmp( id_hash, b_relay->relay.id_hash, H_LENGTH );

    if ( ret < 0 )
    {
      if ( b_relay->avl_blocks[1].left_addr == -1 )
      {
        goto fail;
      }
      else
      {
        next_addr = b_relay->avl_blocks[1].left_addr;
      }
    }
    else if ( ret > 0 )
    {
      if ( b_relay->avl_blocks[1].right_addr == -1 )
      {
        goto fail;
      }
      else
      {
        next_addr = b_relay->avl_blocks[1].right_addr;
      }
    }
    else
    {
      if ( memcmp( identity, b_relay->relay.identity, ID_LENGTH ) == 0 )
      {
        b_relay->relay.is_guard = guard;

        if ( d_store_relay_at_address( b_relay, next_addr ) != 0 )
        {
          goto fail;
        }

        break;
      }
      else
      {
        if ( b_relay->avl_blocks[1].right_addr == -1 )
        {
          goto fail;
        }
        else
        {
          next_addr = b_relay->avl_blocks[1].right_addr;
        }
      }
    }
  }

  free( b_relay );

  return 0;

fail:
  free( b_relay );

  return -1;
}

int d_mark_hsdir_relay_as_guard( uint8_t* identity, uint8_t* id_hash )
{
  return d_update_relay_guard( identity, id_hash, 1 );
}

int d_unmark_hsdir_relay_as_guard( uint8_t* identity, uint8_t* id_hash )
{
  return d_update_relay_guard( identity, id_hash, 0 );
}
