
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <sys/queue.h>

#include "rlib.h"

#define ACK_SIZE 8
#define PACKET_SIZE 500
#define HEADER_SIZE 12

/* ===== Structs ===== */
struct reliable_state {
    rel_t *next; //linked list node
    rel_t **prev;

    conn_t *c;

    // Our data
    int next_in_seq;
    int next_out_seq;
    int last_ack;
};

//Keeps track of packets sent out and waiting for acks
typedef struct out_pkt {
    rel_t *r;
    packet_t *pkt;
    int seqno;
    size_t size;
    struct out_pkt *next;//linked list node
} out_pkt_t;

/* ===== Global variables ===== */
rel_t *rel_list;
out_pkt_t *out_list_head = NULL;

/* ===== Functions ===== */
//Adds a packet to the list of out packets waiting for acks
void add_to_out_list(rel_t* r, packet_t *pkt, int seqno, size_t size) {
    out_pkt_t *to_add = (out_pkt_t*) malloc(sizeof(out_pkt_t));
    to_add->r = r;
    to_add->pkt = pkt;
    to_add->seqno = seqno;
    to_add->size = size;
    to_add->next = NULL;

    if (out_list_head == NULL) {
        out_list_head = to_add;
    }
    else {
        out_pkt_t* temp = out_list_head;
        while (temp->next != NULL)
            temp = temp->next;
        temp->next = to_add;
    }
}

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
            const struct config_common *cc)
{
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
        rel_list->prev = &r->next;
    rel_list = r;

    // Our initialization
    r->next_out_seq = 1;
    r->next_in_seq = 1;
    r->last_ack = -1;

    return r;
}

//Destroy a rel_t
void
rel_destroy (rel_t *r)
{
    if (r->next)
        r->next->prev = r->prev;
    *r->prev = r->next;
    conn_destroy (r->c);

    /* Free any other allocated memory here */
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void
rel_demux (const struct config_common *cc,
         const struct sockaddr_storage *ss,
         packet_t *pkt, size_t len)
{
//  /* demultiplex called when in server mode,
//   * if seq == 1 from a new sockaddr_storage, invoke rel_create else receive
//   */
//  int is_new = 1;
//  rel_t *curr = rel_list;
//
//  while(curr){
//      conn_t * c = curr->c;
//      if( (pkt->seqno != 1) && addreq( ss, &( c->peer ) )){
//          //is existing connection
//          is_new = 0;
//          rel_recvpkt(curr, pkt, len);
//      }
//      curr = curr -> next;
//  }
//  if (is_new){
//      //create new rel_t and receive
//      rel_t * r = rel_create(NULL, ss, cc);
//      rel_recvpkt(r, pkt, len);
//  }

}

// Process a received packet
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    if (n == ACK_SIZE) {
        struct ack_packet* recvd_ack = (struct ack_packet*) pkt;
        r->last_ack = recvd_ack->ackno;
    }
    if (n >= HEADER_SIZE && pkt->seqno == r->next_in_seq) {
        //Print packet data
        int conn_output_return;
        conn_output_return = conn_output(r->c, (void*) pkt->data, n - HEADER_SIZE);

        if (conn_output_return > 0 && conn_output_return < n-HEADER_SIZE){
            // TODO: call conn_output again, passing in portion of the message did not write the first time

        }
        else if (conn_output_return == 0 ){
            //EOF sent
        }
        else if (conn_output_return == -1 ){
            //error
        }

        r->next_in_seq++;

        //Construct ack
        struct ack_packet sent_ack;
        sent_ack.cksum = 0xFFFF;//TODO
        sent_ack.len = ACK_SIZE;
        sent_ack.ackno = r->next_in_seq;

        //Send ack
        conn_sendpkt (r->c, (packet_t*) &sent_ack, ACK_SIZE);
    }
}

//TODO account for window size
// Read user input and send a packet
void
rel_read (rel_t *s)
{
    //Prepare packet
    packet_t *to_send = (packet_t*) malloc(sizeof(packet_t));
    to_send->cksum = 0xFFFF;//TODO
    to_send->ackno = s->next_in_seq;
    to_send->seqno = s->next_out_seq;
    
    //Get user input
    int conn_input_return = conn_input (s->c, (void*) to_send->data, PACKET_SIZE-HEADER_SIZE);
    to_send->len = conn_input_return;

    //Send packet
    if (conn_input_return > -1) {
            conn_sendpkt (s->c, to_send, HEADER_SIZE + conn_input_return);
            add_to_out_list(s, to_send, to_send->seqno, HEADER_SIZE + conn_input_return);
    }
    else if (conn_input_return == 0) {
        //TODO no data currently available
        return;
    }
    else if (conn_input_return == -1) {
        //TODO EOF or error
        return;
    }

    //Increment sequence number
    s->next_out_seq++;
}

void
rel_output (rel_t *r)
{

}

// Retransmit any packets that need to be retransmitted
void
rel_timer ()
{
    //TODO keep track of time elapsed since last try, and only retransmit once timer > timeout
    out_pkt_t *temp = out_list_head;
    while (temp != NULL) {
        if (temp->seqno >= temp->r->last_ack) {
            conn_sendpkt (temp->r->c, temp->pkt, temp->size);
        }
        temp = temp->next;
    }
}
