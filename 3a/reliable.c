
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

#include "rlib.h"

#define PACKET_SIZE 500
#define HEADER_SIZE 12

struct reliable_state {
  rel_t *next;			/* Linked list for traversing all connections */
  rel_t **prev;

  conn_t *c;			/* This is the connection object */

  /* Add your own data fields below this */

};
rel_t *rel_list;





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

  /* Do any other initialization you need here */


  return r;
}

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
}

void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
  if (n >= 12) {
    //Print packet data
    char data[PACKET_SIZE];
    memcpy((void*) &data, (void*) pkt->data, n);
    int i;
    for (i=0; i < n-HEADER_SIZE; i++) {
      printf("%c", (int) data[i]);
    }
    fflush(stdout);
  }
}


void
rel_read (rel_t *s)
{
  //Prepare packet
  packet_t to_send;
  to_send.cksum = 0xFFFF;//TODO
  to_send.ackno = 0xEEEEEEEE;//TODO
  to_send.seqno = 0;//TODO

  //Get user input
  int conn_input_return = conn_input (s->c, (void*) to_send.data, PACKET_SIZE-HEADER_SIZE);
  to_send.len = conn_input_return;

  //Send packet
  if (conn_input_return > -1) {
    conn_sendpkt (s->c, &to_send, HEADER_SIZE + conn_input_return);
  }
  else if (conn_input_return == 0) {
    //no data currently available
  }
  else if (conn_input_return == -1) {
    //EOF or error
  }
}

void
rel_output (rel_t *r)
{
}

void
rel_timer ()
{
  /* Retransmit any packets that need to be retransmitted */

}
