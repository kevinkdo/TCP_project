
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

#define ACK_SIZE 12
#define PACKET_SIZE 1016
#define HEADER_SIZE 16
#define MSS 1000
#define TIMEOUT 100

/* ===== Structs ===== */
struct reliable_state {

	conn_t *c;			/* This is the connection object */

	/* Add your own data fields below this */
	struct timespec *start;

	// sender's view
	uint32_t s_next_out_pkt_seq;      // seqno of next packet to send
	uint32_t s_last_ack_recvd;        // seqno of last packet acked
	uint32_t s_cwnd;						  // congestion window(based on timeout, acks, etc.)
	uint32_t s_rwnd;						  // what receiver says window should be window 
	int s_ssthresh; 		// slow start threshold 
	int s_dup_acks;					  // currently recovering from dup acks
	int s_dup_ack_count;			  // keeps track of how many duplicate acks so far 
	int s_dup_acks_reset;			  // dup ack discovered
	int s_timeout;					  // currently recovering from time out
	int s_timeout_reset;				  // time out discovered
	int send_eof;                // 1 if we have sent eof

	// receiver's view
	uint32_t r_next_exp_seq;            // seqno of next expected packet
	uint32_t r_to_print_pkt_seq;        // when rel_output is called this is the pkt it tries to grab from in_pkt_list
	int recv_eof;                  // 1 if we have received eof

	// Copied from config_common
	int timeout;            // Retransmission timeout in milliseconds	
};
rel_t *rel_list;

// Struct for packets sent out and waiting for acks
typedef struct out_pkt {
	rel_t *r;                   // rel_t associated with this packet
	packet_t *pkt;              // packet that was sent
	uint32_t seqno;             // pkt->seqno
	size_t size;                // UDP length of pkt
	struct timespec *last_try;  // timespec of last send attempt
	struct out_pkt *next;       // linked list node
} out_pkt_t;

// struct for packets that recieved but should not be printed due to previous missing packets
typedef struct in_pkt {
	rel_t *r;                   // rel_t associated with this packet
	packet_t *pkt;              // packet that was received
	uint32_t seqno;             // pkt->seqno
	size_t size;                // UDP length of pkt
	uint16_t progress;		 	// progress (bytes) made in outputting
	uint16_t len;				// length (bytes) for outputting
	struct in_pkt *next;        // linked list node
} in_pkt_t;

/* ===== Global variables ===== */
rel_t *rel_list;
out_pkt_t *out_list_head = NULL;
in_pkt_t *in_list_head = NULL;

/* ===== Functions ===== */
/* From https://tint2.googlecode.com/svn/trunk/src/util/timer.c */
int timespec_subtract(struct timespec* result, struct timespec* x, struct timespec* y)
{
	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_nsec < y->tv_nsec) {
		int nsec = (y->tv_nsec - x->tv_nsec) / 1000000000 + 1;
		y->tv_nsec -= 1000000000 * nsec;
		y->tv_sec += nsec;
	}
	if (x->tv_nsec - y->tv_nsec > 1000000000) {
		int nsec = (x->tv_nsec - y->tv_nsec) / 1000000000;
		y->tv_nsec += 1000000000 * nsec;
		y->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait. tv_nsec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_nsec = x->tv_nsec - y->tv_nsec;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

uint16_t min(uint16_t a, size_t b) {
	if (a < b)
		return a;
	return b;
}

uint32_t min32(uint32_t a, uint32_t b) {
	if (a < b)
		return a;
	return b;
}

void send_ack(rel_t* r) {
	//Construct ack
	struct ack_packet sent_ack;
	sent_ack.cksum = 0x0000;
	sent_ack.len = htons(ACK_SIZE);
	sent_ack.ackno = htonl(r->r_next_exp_seq);
	sent_ack.rwnd = htonl(25);//sent_ack.rwnd = htonl(conn_bufspace(r->c)/MSS);
	sent_ack.cksum = cksum ((void*) &sent_ack, ACK_SIZE);

	//Send ack
	conn_sendpkt (r->c, (packet_t*) &sent_ack, ACK_SIZE);
}

// Returns how much time left (in seconds) until timeout; copied from rlib.c, need_timer_in()
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
void add_to_out_list(rel_t* r, packet_t *pkt, uint32_t seqno, size_t size, struct timespec* timespec) {
	//Construct out_pkt_t
	out_pkt_t *to_add = (out_pkt_t*) malloc(sizeof(out_pkt_t));
	to_add->r = r;
	to_add->pkt = pkt;
	to_add->seqno = seqno;
	to_add->size = size;
	to_add->next = NULL;
	to_add->last_try = timespec;


	//Add to out list
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

void send_eof(rel_t* s) {
	if (s->send_eof == 0) {
		//Make EOF
		packet_t *to_send = (packet_t*) malloc(sizeof(packet_t));
		to_send->cksum = 0x0000;
		to_send->ackno = htonl(s->r_next_exp_seq);
		to_send->seqno = htonl(s->s_next_out_pkt_seq);
		to_send->len = htons(HEADER_SIZE);
		to_send->rwnd = htonl((conn_bufspace(s->c))/MSS);
		to_send->cksum = cksum ((void*) to_send, HEADER_SIZE);

		//Send and add to list
		s->send_eof = 1;
		conn_sendpkt (s->c, to_send, HEADER_SIZE);
		struct timespec *timespec = (struct timespec*) malloc(sizeof(struct timespec));
		clock_gettime (CLOCK_MONOTONIC, timespec);
		add_to_out_list(s, to_send, s->s_next_out_pkt_seq, HEADER_SIZE, timespec);

		//Increment sequence number
		s->s_next_out_pkt_seq++;
	}
}

//Adds a packet to the list of in packets
void add_to_in_list(rel_t* r, packet_t *pkt, size_t size) {
	//Construct in_pkt_t
	in_pkt_t *to_add = (in_pkt_t*) malloc(sizeof(in_pkt_t));
	to_add->r = r;
	to_add->pkt = pkt;
	to_add->seqno = ntohl(pkt->seqno);
	to_add->progress = 0;
	to_add->len = ntohs(pkt->len) - HEADER_SIZE;
	to_add->size = size;
	to_add->next = NULL;

	//Add to in list
	if (in_list_head == NULL) {
		in_list_head = to_add;
	}
	else {
		in_pkt_t* temp = in_list_head;
		while (temp != NULL) {
			if (temp->seqno == to_add->seqno) {
				return;
			}
			if (temp->next == NULL) {
				temp->next = to_add;
				break;
			}
			temp = temp->next;
		}
	}
}

//Finds an in_pkt based on seqno
in_pkt_t* get_in_pkt(uint32_t seqno) {
	in_pkt_t* temp = in_list_head;
	while (temp != NULL) {
		if (temp->seqno == seqno)
			break;
		temp = temp->next;
	}
	return temp;
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
	rel_list = r;

	//Our initialization
	// sender's view
	r->s_next_out_pkt_seq = 1;
	r->s_last_ack_recvd = 1;
	r->s_cwnd = 25;
	r->s_rwnd = 25;
	r->s_ssthresh = cc->window;
	r->s_timeout = 0;
	r->s_timeout_reset = 0;
	r->s_dup_acks = 0;
	r->s_dup_acks_reset = 0;
	r->s_dup_ack_count = 0;
	r->send_eof = 0;

	// receiver's view
	r->r_next_exp_seq = 1;
	r->r_to_print_pkt_seq = 1;
	r->recv_eof = 0;

	// Copied from config_common
	r->timeout = TIMEOUT;

	//Send eof at beginning if RECEIVER
	if (r->c->sender_receiver == RECEIVER) {
		send_eof(r);
	}

	r->start = (struct timespec*) malloc(sizeof(struct timespec));
	clock_gettime (CLOCK_MONOTONIC, r->start);
	return r;
}

void
rel_destroy (rel_t *r)
{
	conn_destroy (r->c);

	/* Free any other allocated memory here */

	struct timespec* end = (struct timespec*) malloc(sizeof(struct timespec));
	clock_gettime (CLOCK_MONOTONIC, end);
	struct timespec* diff = (struct timespec*) malloc(sizeof(struct timespec));
	timespec_subtract(diff, end, r->start);
	fprintf(stderr, "Time elapsed: %ld secs and %ld nanoseconds", diff->tv_sec, diff->tv_nsec);
	fflush(stderr);
}


void 
update_window_size (rel_t *r) {

	if (r->s_timeout_reset == 1) { 
		r->s_dup_acks = 0;
		r->s_timeout = 1;
	}
	if (r->s_dup_acks_reset == 1) {
		r->s_timeout = 0; 
		r->s_dup_acks = 1;
	}

	if (r -> s_dup_acks == 0 && r -> s_timeout == 0 ){
		r -> s_cwnd += 1; //AI

	} else if (r->s_dup_acks == 1) {
		if (r->s_dup_acks_reset == 1) {
			r->s_ssthresh = r->s_cwnd/2;
			r->s_cwnd = r->s_ssthresh;
			r->s_dup_acks_reset = 0;
		} else {
			r->s_cwnd += 1;
		} 

		//end of recovery from dup acks
		if (r -> s_cwnd >= r -> s_ssthresh){
			r -> s_dup_acks = 0;
		}
		
	} else if (r->s_timeout == 1) {
		if (r->s_timeout_reset == 1) {
			r->s_ssthresh = r->s_cwnd/2;
			r->s_cwnd = 1;
			r->s_timeout_reset = 0;
		} else if (r->s_ssthresh > r->s_cwnd) {
			r->s_cwnd *= 2; 		//slow start 
		} else {
			r->s_cwnd+= 1; 		//AIMD
		}

		//end of recovery from timeout
		if (r -> s_cwnd >= r -> s_ssthresh) {
			r -> s_timeout = 0;
		}
	}
}


// Process a received packet
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
	// Verify checksum; abort if necessary
	uint16_t cksum_recv = pkt->cksum;
	pkt->cksum = 0x0000;
	uint16_t cksum_calc = cksum ((void*) pkt, min(ntohs(pkt->len), n));
	if (cksum_recv != cksum_calc) {
		send_ack(r);
		return;
	}

	// Update s_last_ack_recvd for sender state
	if (ntohl(pkt->ackno) > r->s_last_ack_recvd && ntohl(pkt->ackno) <= r->s_next_out_pkt_seq){
		//Check for dup acks
		if (ntohl(pkt->ackno) == r->s_last_ack_recvd) { 
			r->s_dup_ack_count++;
			if (r->s_dup_ack_count == 3) {
				r->s_dup_acks_reset = 1;
				r->s_dup_ack_count = 0;
			}
		} else {
			r->s_dup_ack_count = 0; 
		}
		r->s_last_ack_recvd = ntohl(pkt->ackno);
	}
	
	//update window size
	r->s_rwnd = ntohl(pkt->rwnd);
	update_window_size(r);

	// Received data packet
	if (n >= HEADER_SIZE && ntohs(pkt->len) >= HEADER_SIZE && ntohl(pkt->seqno) == r->r_next_exp_seq) {
		// Discard garbage pkt, out of the receiving window
		if (ntohl(pkt->seqno) < r->r_next_exp_seq ||
			ntohl(pkt->seqno) >= (r->r_next_exp_seq + (conn_bufspace(r->c))/MSS)) {
			send_ack(r);
			return;
		}

		// add to in_pkt_list
		add_to_in_list(r, pkt, n);

		// update r_next_exp_seq
		while (get_in_pkt(r->r_next_exp_seq) != NULL)
			r->r_next_exp_seq++;

		//Received EOF
		if (ntohs(pkt->len) == HEADER_SIZE) {
			r->recv_eof = 1;
			send_ack(r);
		}


		// Try to output
		rel_output(r);
	}
}

void
rel_read (rel_t *s)
{
	if (s->c->sender_receiver == RECEIVER) {
		send_eof(s);
	}
	else {
		//Prepare packet
		packet_t *to_send = (packet_t*) malloc(sizeof(packet_t));
		to_send->cksum = 0x0000;
		to_send->ackno = htonl(s->r_next_exp_seq);
		to_send->seqno = htonl(s->s_next_out_pkt_seq);
		to_send->rwnd = htonl((conn_bufspace(s->c))/MSS);

		//Get user input
		int conn_input_return = conn_input (s->c, (void*) to_send->data, PACKET_SIZE-HEADER_SIZE);

		//User entered data
		if (conn_input_return > -1) {
			//Calculate fields
			to_send->len = htons(conn_input_return + HEADER_SIZE);
			to_send->cksum = cksum ((void*) to_send, HEADER_SIZE + conn_input_return);

			//Send if possible
			if (s->s_next_out_pkt_seq - s->s_last_ack_recvd < min32(s->s_cwnd, s->s_rwnd))
				conn_sendpkt (s->c, to_send, HEADER_SIZE + conn_input_return);

			struct timespec *timespec = (struct timespec*) malloc(sizeof(struct timespec));
			clock_gettime (CLOCK_MONOTONIC, timespec);
			add_to_out_list(s, to_send, s->s_next_out_pkt_seq, HEADER_SIZE + conn_input_return, timespec);
		}
		//No data currently available
		else if (conn_input_return == 0) { 
			return;
		}
		//Send EOF
		else if (conn_input_return == -1) {
			//Calculate fields
			to_send->len = htons(HEADER_SIZE);
			to_send->cksum = cksum ((void*) to_send, HEADER_SIZE);

			//Record
			s->send_eof = 1;

			//Send and add to list
			conn_sendpkt (s->c, to_send, HEADER_SIZE);
			struct timespec *timespec = (struct timespec*) malloc(sizeof(struct timespec));
			clock_gettime (CLOCK_MONOTONIC, timespec);
			add_to_out_list(s, to_send, s->s_next_out_pkt_seq, HEADER_SIZE, timespec);
		}

		//Increment sequence number
		s->s_next_out_pkt_seq++;
	}
}

//Output received data
void
rel_output (rel_t *r)
{
	int conn_output_return = 1;
	while (conn_output_return > 0) {
		//Look for packet to output
		in_pkt_t* temp = get_in_pkt(r->r_to_print_pkt_seq);

		//Ack for packet we looked for
		if (temp == NULL) {
			send_ack(r);
			return;
		}

		//Try to output
		conn_output_return = conn_output(r->c, (void*)temp->pkt->data, temp->len - temp->progress);

		//Record progress
		if (conn_output_return > 0) {
			temp->progress += conn_output_return;
		}
		if (conn_output_return == 0) {}
		if (conn_output_return == -1) {}

		//If done with this packet, move on to next packet
		if (temp->progress == temp->len) {
			r->r_to_print_pkt_seq++;
		}
	}
}

void
clean_in_pkt_list(){
	//clean in_pkt_list
	in_pkt_t* curr = in_list_head;
	in_pkt_t* prev = NULL;
	while(curr){
		if ( curr-> seqno < curr -> r -> r_to_print_pkt_seq ){
			//already printed pkt, remove
			if (!prev) {
				// remove head
				in_pkt_t* to_free = curr;
				in_list_head = curr -> next;
				curr = in_list_head;
				free(to_free);

			} else {
				in_pkt_t* to_free = curr;
				curr = curr -> next;
				prev -> next = curr;
				free(to_free);
			}

		} else {
			prev = curr;
			curr = curr -> next;
		}
	} 
}


// Retransmit any packets that need to be retransmitted
void
rel_timer () {
	out_pkt_t *temp = out_list_head;
	out_pkt_t *prev = NULL;
	while (temp) {

		// If unacked + window is satisfied + timeout, resend
		if (temp->seqno >= temp->r->s_last_ack_recvd &&
				temp->seqno - temp->r->s_last_ack_recvd < min32(temp->r->s_cwnd, temp->r->s_rwnd) &&
				time_until_timeout(temp->last_try, (long) temp->r->timeout) == 0)
		{
			conn_sendpkt (temp->r->c, temp->pkt, temp->size);
			clock_gettime(CLOCK_MONOTONIC, temp->last_try);
			//temp->r->s_timeout_reset = 1;
			update_window_size(temp->r);
		}

		//remove the pkt if needed, otherwise just move on
		if (temp -> seqno < temp -> r -> s_last_ack_recvd){
			if (!prev) {
				// remove head
				out_pkt_t* to_free = temp;
				out_list_head = temp -> next;
				temp = out_list_head;
				free(to_free);
			} else {
				// remove node
				out_pkt_t* to_free = temp;
				temp = temp -> next;
				prev -> next = temp;
				free(to_free);
			}

		} else {
			//nothing to remove, move on
			prev = temp;
			temp = temp->next;
		}

	}

	//clean in_pkt_list
	clean_in_pkt_list();

	//If necessary, close connection
	rel_t *temp_rel = rel_list;
	if (temp_rel -> send_eof > 0
		&& temp_rel -> recv_eof > 0
		&& temp_rel->s_last_ack_recvd == temp_rel->s_next_out_pkt_seq
		&& temp_rel->r_to_print_pkt_seq == temp_rel->r_next_exp_seq) {
		rel_destroy(temp_rel);
	}
}

void
rel_demux (const struct config_common *cc,
		 const struct sockaddr_storage *ss,
		 packet_t *pkt, size_t len)
{}