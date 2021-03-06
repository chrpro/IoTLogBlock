/*
 * Copyright (c) 2019, Christos Profentzas - www.chalmers.se/~chrpro 
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "contiki.h"
#include "random.h"
#include "net/ipv6/simple-udp.h"
#include "dev/rom-util.h"
#include "dev/sha256.h"
#include "dev/ecc-algorithm.h"
#include "dev/ecc-curve.h"
#include "sys/rtimer.h"
#include "sys/pt.h"
#include "sys/energest.h"
#include "sys/log.h"
#include <stdbool.h>
#include "Offchain-message.h"

// #define CONSTANT_CONNECTIVITY
#define MEASURE_ENERGY

#define LOG_MODULE "Chain-Orig"
#define LOG_LEVEL LOG_LEVEL_DBG
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678
#define MAX_TXS 30

//REV reversve the byte order
#define REV(X) ((X << 24) | ((X & 0xff00) << 8) | ((X >> 8) & 0xff00) | (X >> 24))
#define GENERATION_INTERVAL (10 * CLOCK_SECOND)
#define START_INTERVAL (0.001 * CLOCK_SECOND)
#define TX_INTERVAL (90 * CLOCK_SECOND)
#define EDGE_CONNECTION (30 * CLOCK_SECOND)
#define RESOLVE_TIMEOUT (120 * CLOCK_SECOND)
#define ABORD_TIMEOUT (120 * CLOCK_SECOND)
#define NODE_ID 1

static int memory_max = 0;
static uint16_t droped_rec = 0;
static const uint16_t node_id = NODE_ID;
static bool tx_free = true;
static u_int32_t record_counter = 0;
static struct simple_udp_connection udp_conn;

#ifdef MEASURE_ENERGY
static unsigned long
to_seconds(uint64_t time)
{
  return (unsigned long)(time / ENERGEST_SECOND);
}
#endif

static uint32_t buffer_counter = 0;
static msg_record buffer[MAX_TXS];
static msg_header msg_buffer;
static msg_record complete_rec;
static uip_ipaddr_t dest_iot_node = {{0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}};

/*---------------------------------------------------------------------------*/
PROCESS(chain_client_process, "Chain client");
AUTOSTART_PROCESSES(&chain_client_process);
/*---------------------------------------------------------------------------*/

static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  msg_header *msg_rcv = (struct msg_header *)data;
  // printf("MSg recvd: %d\n", msg_rcv->type);

  switch (msg_rcv->type)
  {
  // An exchange message means the other end is ready for Transcaction
  case MSG_TYPE_READY:
  {
    // check if there is ongoing transcation
    if (tx_free == true)
    {
      // new transction , save ip of other node
      memcpy(&dest_iot_node, sender_addr, sizeof(uip_ipaddr_t));
      memcpy(&msg_buffer, msg_rcv, sizeof(msg_header));
      // A P-thread needed to  sing transctions
      // threads can spawn only by processed
      // thus we send a msg_event to main process to handle it
      process_post(&chain_client_process, PROCESS_EVENT_MSG, (process_data_t)&msg_buffer);
      tx_free = false;
    }
    // in case ongoing transction , do nothing , a timer handles failure
  }
  break;

  //the Responder send the m2
  case MSG_TYPE_M2:
  {
    memcpy(&msg_buffer, msg_rcv, sizeof(msg_rcv));
    process_post(&chain_client_process, PROCESS_EVENT_MSG, (process_data_t)&msg_buffer);
  }
  break;

  case MSG_TYPE_M4:
  {
    memcpy(&msg_buffer, msg_rcv, sizeof(msg_rcv));
    process_post(&chain_client_process, PROCESS_EVENT_MSG, (process_data_t)&msg_buffer);
  }
  break;

  default: //Optional
  {
    printf("Undifined msg type\n");
  }
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(chain_client_process, ev, data)
{
  static struct etimer periodic_hello, edge_connection;
  static unsigned count;
  static msg_m1 reply_m1;
  static msg_m3 reply_m3;
  static short nonce;
  static sha256_state_t sha256_state;
  static uint8_t sha256_digest[32];
  static uint8_t ret;
  static uint32_t len;

  static ecc_dsa_sign_state_t sign_state = {
      .process = &chain_client_process,
      .curve_info = &nist_p_256,
      .secret = {0x94A949FA, 0x401455A1, 0xAD7294CA, 0x896A33BB,
                 0x7A80E714, 0x4321435B, 0x51247A14, 0x41C1CB6B},
      .k_e = {0x1D1E1F20, 0x191A1B1C, 0x15161718, 0x11121314,
              0x0D0E0F10, 0x090A0B0C, 0x05060708, 0x01020304},
  };
  static rtimer_clock_t time;
  static rtimer_clock_t total_time;

  // node ready for transaction in the start
  tx_free = true;

  PROCESS_BEGIN();
  /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL, UDP_SERVER_PORT, udp_rx_callback);
  etimer_set(&periodic_hello, START_INTERVAL);
  etimer_set(&edge_connection, EDGE_CONNECTION);

  while (1)
  {
    
    PROCESS_WAIT_EVENT(); 
    
// Update all energest times.
#ifdef MEASURE_ENERGY
    energest_flush();

    printf("\nEnergest:\n");
    printf(" CPU          %4lus LPM      %4lus DEEP LPM %4lus  Total time %lus\n",
           to_seconds(energest_type_time(ENERGEST_TYPE_CPU)),
           to_seconds(energest_type_time(ENERGEST_TYPE_LPM)),
           to_seconds(energest_type_time(ENERGEST_TYPE_DEEP_LPM)),
           to_seconds(ENERGEST_GET_TOTAL_TIME()));
    printf(" Radio LISTEN %4lus TRANSMIT %4lus OFF      %4lus\n",
           to_seconds(energest_type_time(ENERGEST_TYPE_LISTEN)),
           to_seconds(energest_type_time(ENERGEST_TYPE_TRANSMIT)),
           to_seconds(ENERGEST_GET_TOTAL_TIME() - energest_type_time(ENERGEST_TYPE_TRANSMIT) - energest_type_time(ENERGEST_TYPE_LISTEN)));
#endif

    // The udp callback send an event for messages related to AWP protocol:
    // This is because the cyrpto funcations need to handled as a thread

    // printf(" memory_max : %d\n",memory_max);
    if (ev == PROCESS_EVENT_MSG)
    {
      //select through the type of msg
      msg_header *msg_rcv = (struct msg_header *)data;
      // nodes said hello -> ready for exchange
      if (msg_rcv->type == MSG_TYPE_READY)
      {
        record_counter++;
        printf("<rec_timestamp>%ld\n", record_counter);
        //start measure the time for protocol to complete
        total_time = RTIMER_NOW();

#ifdef ABORT_SUB_PROTOCOL
        //start a transation process
        etimer_set(&abord_period, ABORD_TIMEOUT);
#endif
        //IMPORTANT! neet to init crypto coprocessor before use
        crypto_init();
        pka_init();
        uint32_t *ptr = (uint32_t *)&sha256_digest;

        reply_m1.type = MSG_TYPE_M1;
        reply_m1.context.originator_id = 1;
        reply_m1.context.responder_id = 2;
        reply_m1.context.smart_contract_id = 3;
        reply_m1.context.record_id = 4;
        nonce = random_rand();
        len = sizeof(short);

        time = RTIMER_NOW();
        //Check if the initialization was successfull
        ret = sha256_init(&sha256_state);
        ret = sha256_process(&sha256_state, &nonce, len);
        ret = sha256_done(&sha256_state, sha256_digest);

        ptr = (uint32_t *)&sha256_digest;
        int i;
        int j = 7;
        for (i = 0; i < 8; i++)
        {
          reply_m1.context.hash_nonce_o[i] = REV(ptr[j]);
          ;
          j--;
        }

        len = sizeof(struct msg_contex);
        time = RTIMER_NOW();
        ret = sha256_init(&sha256_state);
        ret = sha256_process(&sha256_state, &reply_m1.context, len);
        printf("sha256_process(): %d\n", ret);
        ret = sha256_done(&sha256_state, sha256_digest);

        time = RTIMER_NOW() - time;
        printf("sha256 - reply.body.me1 ,  time: , %lu ms\n",
               (uint32_t)((uint64_t)time * 1000 / RTIMER_SECOND));


        // This is how should print the hash to test from other program
        // in contiki the hash is used in revered bit-order
        printf("\nHash of signature: \n");

        ptr = (uint32_t *)&sha256_digest;
        j = 7;
        for (i = 0; i < 8; i++)
        {
          sign_state.hash[i] = REV(ptr[j]);
          printf("%08lx", REV(ptr[i]));
          j--;
        }
        printf("\n-------------------\n");
        time = RTIMER_NOW();
        PT_SPAWN(&(chain_client_process.pt), &(sign_state.pt), ecc_dsa_sign(&sign_state));
        time = RTIMER_NOW() - time;
        printf("sing of origin msg,  time: , %lu ms\n",
               (uint32_t)((uint64_t)time * 1000 / RTIMER_SECOND));

        reply_m1.point_r = sign_state.point_r;
        memcpy(reply_m1.signature_o, sign_state.signature_s, sizeof(uint32_t) * 24);
        memcpy(&complete_rec, &reply_m1, sizeof(msg_m1));

        //copy the reply message to global  buffer complete_tx

        complete_rec.nonce_o = nonce;

        crypto_disable();
        pka_disable();
        simple_udp_sendto(&udp_conn, &reply_m1, sizeof(struct msg_m1), &dest_iot_node);
      }
      else if (msg_rcv->type == MSG_TYPE_M2)
      {
        //responder send m2
        // printf("got m2\n");
        //stop abort timeout
        // etimer_stop(&abord_period);
        //start resolving timeout
        // etimer_set(&resolve_period, RESOLVE_TIMEOUT);
        memcpy(&complete_rec.m2, msg_rcv, sizeof(msg_m2));
        reply_m3.type = MSG_TYPE_M3;
        reply_m3.nonce = nonce;
        simple_udp_sendto(&udp_conn, &reply_m3, sizeof(struct msg_m3), &dest_iot_node);
      }
      else if (msg_rcv->type == MSG_TYPE_M4)
      {
        // printf("nonce of respond:%d\n", ((msg_m4 *)msg_rcv)->nonce);

        crypto_init();
        sha256_state_t state;
        uint8_t sha256[32];
        uint32_t sha256_digest[8];
        int len = sizeof(short);
        ret = sha256_init(&state);
        ret = sha256_process(&state, &((msg_m4 *)msg_rcv)->nonce, len);
        ret = sha256_done(&state, sha256);
        // printf("sha256_process(): %s\n", str_res[ret]);
        crypto_disable();
        uint32_t *ptr = (uint32_t *)&sha256;
        int i;
        int j = 7;
        for (i = 0; i < 8; i++)
        {
          sha256_digest[i] = REV(ptr[j]);
          // printf("%08lx", REV(ptr[j]));
          j--;
        }

        if (rom_util_memcmp(sha256_digest, &complete_rec.nonce_r, sizeof(sha256)))
        {
          printf("NonceR not match");
        }
        else
        {
          printf("NonceR hash OK\n");
        }
        puts("----------------");


        total_time = RTIMER_NOW() - total_time;
        printf("procol overall time: , %lu ms\n",
               (uint32_t)((uint64_t)total_time * 1000 / RTIMER_SECOND));
        tx_free = true;
        complete_rec.nonce_r = ((msg_m4 *)msg_rcv)->nonce;
        complete_rec.status = STATUS_COMPLETE;
        complete_rec.rec_counter = record_counter;
        if (buffer_counter < MAX_TXS)
        {
          buffer[buffer_counter] = complete_rec;
          buffer_counter++;
          printf("Buffer counter : %ld\n", buffer_counter);
        }
        else
        {
          droped_rec++;
          printf("Record Droped :%d \n", droped_rec);
        }
        if (buffer_counter > memory_max)
        {
          memory_max = buffer_counter;
        }
#ifdef CONSTANT_CONNECTIVITY
        printf("<transcation>\n");
        printf("%d\n", complete_rec.rec_counter);
        printf("%d\n", node_id);
        struct msg_record *strucPtr = &complete_rec;
        unsigned char *charPtr = (unsigned char *)strucPtr;
        for (i = 0; i < sizeof(struct msg_record); i++)
        {
          printf("%02x", charPtr[i]);
        }
        printf("\n");
        //hex hash of nonce of respond
        uint32_t *hashPtr = complete_rec.m2.hash_nonce_r;
        charPtr = (unsigned char *)hashPtr;
        // printf("structure size : %d bytes\n", sizeof(struct msg_transction));
        // printf("\n");//hex:
        for (i = 0; i < sizeof(uint32_t) * 8; i++)
          printf("%02x", charPtr[i]);

        printf("\n");

        // signature
        for (i = 7; i >= 0; i--)
        {
          printf("%08lx", complete_rec.m1.signature_o[i]);
        }
        printf("\n");
        // signature
        for (i = 7; i >= 0; i--)
        {
          printf("%08lx", complete_rec.m2.signature_r[i]);
        }
        printf("\n");
        //Nonce of Origin
        printf("%d", complete_rec.nonce_o);
        printf("\n");
        //Nonce of Responder
        printf("%d", complete_rec.nonce_r);
        printf("\n");
        printf("</transcation>\n");
#endif
      }
    }
    else if (ev == PROCESS_EVENT_TIMER)
    {
      if (etimer_expired(&periodic_hello))
      {

        msg_header to_send;
        to_send.type = MSG_TYPE_HELLO;
        printf("\nSending request %u ", count);
        printf("\n");
        simple_udp_sendto(&udp_conn, &to_send, sizeof(struct msg_header), &dest_iot_node);
        count++;
        /* Add some jitter */
        etimer_set(&periodic_hello, GENERATION_INTERVAL - CLOCK_SECOND + (random_rand() % (2 * CLOCK_SECOND)));
      }
      if (etimer_expired(&edge_connection))
      {
        // printf("Edge periodic connection\n");
        etimer_set(&edge_connection, EDGE_CONNECTION);

        if (buffer_counter > 0)
        {
#ifndef CONSTANT_CONNECTIVITY
          int transmit_record;
          for (transmit_record = 0; transmit_record < buffer_counter; transmit_record++)
          {

            printf("<transcation>\n");
            printf("%d\n", buffer[transmit_record].rec_counter);
            printf("%d\n", node_id);
            struct msg_record *strucPtr = &buffer[transmit_record];
            unsigned char *charPtr = (unsigned char *)strucPtr;
            int i;
            for (i = 0; i < sizeof(struct msg_record); i++)
            {
              printf("%02x", charPtr[i]);
            }
            printf("\n");
            //Hex hash of nonce of respond
            uint32_t *hashPtr = buffer[transmit_record].m2.hash_nonce_r;
            charPtr = (unsigned char *)hashPtr;
            for (i = 0; i < sizeof(uint32_t) * 8; i++){
              printf("%02x", charPtr[i]);
            }
            printf("\n");

            //Signature of Originator
            for (i = 7; i >= 0; i--)
            {
              printf("%08lx", buffer[transmit_record].m1.signature_o[i]);
            }
            printf("\n");
            //Signature of Responder
            for (i = 7; i >= 0; i--)
            {
              printf("%08lx", buffer[transmit_record].m2.signature_r[i]);
            }
            printf("\n");
            //Nonce of Origin
            printf("%d", buffer[transmit_record].nonce_o);
            printf("\n");
            //Nonce of Responder
            printf("%d", buffer[transmit_record].nonce_r);
            printf("\n");
            printf("</transcation>\n");
          }
#endif
          buffer_counter = 0;
        }
      }

//Abort sub-prtoocol
#ifdef ABORT_SUB_PROTOCOL
      if (etimer_expired(&abord_period))
      {
        printf("Abort Sub-Protocol Timer Expired!\n");
        //clear transaction
        //check flag - > abord the protocol
        complete_rec.status = STATUS_ABORT;
        buffer[buffer_counter] = complete_rec;
        buffer_counter++;
        tx_free = true;
      }
#endif

//Resolve sub-protocol
#ifdef RESOLVE_PROTOCOL
      if (etimer_expired(&resolve_period))
      {
        // sing protol is incomplete
        printf("Resolce Sub-Protocol\n");
        //clear transaction
        //check flag - > abord the protocol
        complete_rec.status = STATUS_RESOLVE;
        buffer[buffer_counter] = complete_rec;
        buffer_counter++;
        tx_free = true;
      }
#endif
    }
  }

  PROCESS_END();
}