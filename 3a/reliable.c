
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
    rel_t *next;            // linked list node
    rel_t **prev;

    conn_t *c;              // connection object

    // Our data
    int next_in_seq;        // seqno of next expected packet
    int next_out_seq;       // seqno of next packet to send
    int last_ack;           // seqno of last packet acked
    int send_eof;           // 1 if we have sent eof
    int recv_eof;           // 0 if we have received eof

    // Copied from config_common
    int window;             // # of unacknowledged packets in flight
    int timeout;            // Retransmission timeout in milliseconds
};

//Keeps track of packets sent out and waiting for acks
typedef struct out_pkt {
    rel_t *r;                   // rel_t associated with this packet
    packet_t *pkt;              // packet that was sent
    int seqno;                  // pkt->seqno
    size_t size;                // length of pkt
    struct timespec *last_try;  // timespec of last send attempt
    struct out_pkt *next;       // linked list node
} out_pkt_t;

/* ===== Global variables ===== */
rel_t *rel_list;
out_pkt_t *out_list_head = NULL;

/* ===== Functions ===== */
// Returns how much time left (in seconds) until timeout
long time_until_timeout (const struct timespec *last, long timeout) {
    long to;
    struct timespec ts;

    clock_gettime (CLOCK_MONOTONIC, &ts);
    to = ts.tv_sec - last->tv_sec;
    if (to > timeout / 1000)
        return 0;
    to = to * 1000 + (ts.tv_nsec - last->tv_nsec) / 1000000;
    if (to >= timeout)
        return 0;
    return
        timeout - to;
}

//Adds a packet to the list of out packets waiting for acks
void add_to_out_list(rel_t* r, packet_t *pkt, int seqno, size_t size, struct timespec* timespec) {
    out_pkt_t *to_add = (out_pkt_t*) malloc(sizeof(out_pkt_t));
    to_add->r = r;
    to_add->pkt = pkt;
    to_add->seqno = seqno;
    to_add->size = size;
    to_add->next = NULL;
    to_add->last_try = timespec;

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
    r->last_ack = 1;

    r->window = cc->window;
    r->timeout = cc->timeout;

    r->send_eof = 0;
    r->recv_eof = 0;

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
    // TODO
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
    // Verify checksum; abort if necessary
    uint16_t cksum_recv = pkt->cksum;
    pkt->cksum = 0x0000;
    uint16_t cksum_calc = cksum ((void*) pkt, pkt->len);
    if (cksum_recv != cksum_calc)
        return;

    //Received ack
    if (pkt->len == ACK_SIZE) {
        struct ack_packet* recvd_ack = (struct ack_packet*) pkt;
        r->last_ack = recvd_ack->ackno;
    }

    //Common code for EOF and data packet
    if (pkt->len >= HEADER_SIZE) {
        //Update ackno
        r->next_in_seq++;

        //Construct ack
        struct ack_packet sent_ack;
        sent_ack.cksum = 0x0000;
        sent_ack.len = ACK_SIZE;
        sent_ack.ackno = r->next_in_seq;
        sent_ack.cksum = cksum ((void*) &sent_ack, ACK_SIZE);

        //Send ack
        conn_sendpkt (r->c, (packet_t*) &sent_ack, ACK_SIZE);
    }

    //Received EOF
    if (pkt->len == HEADER_SIZE) {
        r->recv_eof = 1;
    }

    //Received data packet
    if (pkt->len > HEADER_SIZE && pkt->seqno == r->next_in_seq) {
        //Print packet data
        int conn_output_return;
        conn_output_return = conn_output(r->c, (void*) pkt->data, n - HEADER_SIZE);

        if (conn_output_return > 0 && conn_output_return < n-HEADER_SIZE){
            // TODO: call conn_output again, passing in portion of the message did not write the first time

        }
        else if (conn_output_return == 0 ) {
            
        }
        else if (conn_output_return == -1 ) {
            //error
        }
    }
}

//TODO To conserve packets, a sender should not send more than one unacknowledged Data frame with less than the maximum number of packets (500), somewhat like TCP's Nagle algorithm.
// Read user input and send a packet
void
rel_read (rel_t *s)
{
    //Prepare packet
    packet_t *to_send = (packet_t*) malloc(sizeof(packet_t));
    to_send->cksum = 0x0000;
    to_send->ackno = s->next_in_seq;
    to_send->seqno = s->next_out_seq;
    
    //Get user input
    int conn_input_return = conn_input (s->c, (void*) to_send->data, PACKET_SIZE-HEADER_SIZE);
    to_send->len = conn_input_return + HEADER_SIZE;

    //Calculate checksum
    to_send->cksum = cksum ((void*) to_send, HEADER_SIZE + conn_input_return);

    //Send if possible; add to list
    if (conn_input_return > -1) {
            if (s->next_out_seq - s->last_ack < s->window)
                conn_sendpkt (s->c, to_send, HEADER_SIZE + conn_input_return);

            struct timespec *timespec = (struct timespec*) malloc(sizeof(struct timespec));
            clock_gettime (CLOCK_MONOTONIC, timespec);
            add_to_out_list(s, to_send, to_send->seqno, HEADER_SIZE + conn_input_return, timespec);
    }
    //No data currently available
    else if (conn_input_return == 0) { 
        return;
    }
    //Send EOF
    else if (conn_input_return == -1) {
        //Recalculate fields
        to_send->cksum = 0x0000;
        to_send->len = HEADER_SIZE;
        to_send->cksum = cksum ((void*) to_send, HEADER_SIZE);

        //Record
        s->send_eof = 1;

        //Send and add to list
        conn_sendpkt (s->c, to_send, HEADER_SIZE);
        struct timespec *timespec = (struct timespec*) malloc(sizeof(struct timespec));
        clock_gettime (CLOCK_MONOTONIC, timespec);
        add_to_out_list(s, to_send, to_send->seqno, HEADER_SIZE, timespec);
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
rel_timer () {
    out_pkt_t *temp = out_list_head;
    while (temp) {
        //If unacked + window is satisfied + timeout, resend
        if (temp->seqno >= temp->r->last_ack &&
            temp->seqno - temp->r->last_ack < temp->r->window && 
            time_until_timeout(temp->last_try, (long) temp->r->timeout) == 0)
        {
            conn_sendpkt (temp->r->c, temp->pkt, temp->size);
            clock_gettime(CLOCK_MONOTONIC, temp->last_try);
        }
        temp = temp->next;
    }

    rel_t *temp_rel = rel_list;
    while (temp_rel) {
        if (temp_rel -> send_eof > 0 && temp_rel -> recv_eof > 0)
            rel_destroy(temp_rel);
        temp_rel = temp_rel->next;
    }
}
