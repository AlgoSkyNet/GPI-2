/*
Copyright (c) Fraunhofer ITWM - Carsten Lojewski <lojewski@itwm.fhg.de>, 2013-2014

This file is part of GPI-2.

GPI-2 is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License
version 3 as published by the Free Software Foundation.

GPI-2 is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GPI-2. If not, see <http://www.gnu.org/licenses/>.
*/

#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "GPI2.h"
#include "GPI2_SN.h"
#include "utils.h"
#include "tcp_device.h"
#include "list.h"

tcp_dev_conn_state_t **rank_state = NULL;

/* list of remote operations */
list delayedList = 
  {
    .first = NULL,
    .last = NULL,
    .count = 0
  };

/* list of recvd WRs */
list recvList = 
  {
    .first = NULL,
    .last = NULL,
    .count = 0
  };

/* TODO: to remove? */ 
int cq_ref_counter = 0;
int qs_ref_counter = 0;

struct tcp_cq *cqs_map[CQ_MAX_NUM];

struct tcp_passive_channel *
tcp_dev_create_passive_channel(void)
{
  int pipefd[2];
  struct tcp_passive_channel * channel = NULL;

  if(pipe(pipefd) < 0)
    return NULL;

  channel = (struct tcp_passive_channel *) malloc (sizeof(struct tcp_passive_channel));
  if(channel != NULL)
    {
      channel->read = pipefd[0];
      channel->write = pipefd[1];
    }
  
  return channel;
}

int
tcp_dev_is_valid_state(gaspi_rank_t i)
{
  if(rank_state != NULL)
    {
      if(rank_state[i] != NULL)
	return 1;
    }

  return 0;
}




void
tcp_dev_destroy_passive_channel(struct tcp_passive_channel *channel)
{
  if(channel != NULL)
    {
      close(channel->read);
      close(channel->write);

      free(channel);
    }
}

struct tcp_cq *
tcp_dev_create_cq(int elems, struct tcp_passive_channel *pchannel)
{
  if(elems > CQ_MAX_SIZE)
    {
      gaspi_dev_print_error("Too many elems for completion.");
      return NULL;
    }

  if(cq_ref_counter > CQ_MAX_NUM)
    {
      gaspi_dev_print_error("Reached max number of CQs.");
      return NULL;
    }

  struct tcp_cq *cq = (struct tcp_cq *) malloc(sizeof(struct tcp_cq));
  if(cq == NULL)
    {
      printf("Failed to alloc memory for completion queue\n");
      return NULL;
    }
  
  ringbuffer *rb = (ringbuffer *) malloc(sizeof(ringbuffer));
  if(rb == NULL)
    {
      printf("Failed to alloc memory for completion queue\n");
      free(cq);
      return NULL;
    }

  rb->cells = (rb_cell *) malloc( (elems * 2 + 1) * sizeof(rb_cell));
  if(rb->cells == NULL)
  {
    gaspi_dev_print_error("Failed to alloc memory for completion queue elems (%d).", elems);
    free(rb);
    free(cq);
    return NULL;
  }

  rb->mask = elems + 1;
  rb->ipos = 0;
  rb->rpos = 0;

  cq->rbuf = rb;
  cq->num = cq_ref_counter;
  cq->pchannel = pchannel;
  
  cqs_map[cq_ref_counter++] = cq;
  
  return cq;
}

void
tcp_dev_destroy_cq(struct tcp_cq *cq)
{
  if(cq != NULL)
    {
      if(cq->rbuf->cells != NULL)
	{
	  free(cq->rbuf->cells);
	  cq->rbuf->cells = NULL;
	}
      if(cq->rbuf != NULL)
	free(cq->rbuf);

      free(cq);
    }
}

struct tcp_queue *
tcp_dev_create_queue(struct tcp_cq *send_cq, struct tcp_cq *recv_cq)
{
  int handle = -1;
  
  if(qs_ref_counter >= QP_MAX_NUM)
    {
      gaspi_dev_print_error("Too many created queues.");
      return NULL;
    }
  
  struct tcp_queue *q = (struct tcp_queue *) malloc(sizeof(struct tcp_queue));
  if(q != NULL)
    {
      handle = gaspi_connect2port("localhost", TCP_DEV_PORT + glb_gaspi_ctx.localSocket, CONN_TIMEOUT);
      
      if(handle == -1)
	return NULL;

      q->handle = handle;
      q->send_cq = send_cq;
      q->recv_cq = recv_cq;
      q->num = qs_ref_counter++;
    }

  return q;
}

void
tcp_dev_destroy_queue(struct tcp_queue *q)
{
  if(q != NULL)
    {
      close(q->handle);
      free(q);
      qs_ref_counter--;
    }
}



/* Allocate memory to maintain socket state for remote ranks */
static int
_tcp_dev_alloc_remote_states (int n)
{
  int j;
  
  /* might already have been allocated */
  if(rank_state != NULL)
    return 0;

  rank_state = (tcp_dev_conn_state_t **) malloc(n * sizeof(tcp_dev_conn_state_t *));
  if(rank_state == NULL)
   {
      gaspi_dev_print_error("Failed to allocate memory");
      return 1;
    }

  for(j = 0; j < n; j++)
    {
      rank_state[j] = NULL;
    }

  return 0;
}

static inline tcp_dev_conn_state_t *
_tcp_dev_add_new_conn(int rank, int conn_sock, int epollfd)
{
  tcp_dev_conn_state_t *nstate = (tcp_dev_conn_state_t *) malloc(sizeof(tcp_dev_conn_state_t));
  if(nstate == NULL)
    {
      close(conn_sock);
      return NULL;
    }
      
  nstate->fd              = conn_sock;
  nstate->rank            = rank;
  nstate->read.wr_id      = 0;
  nstate->read.cq_handle  = CQ_HANDLE_NONE;
  nstate->read.opcode     = RECV_HEADER;
  nstate->read.addr       = (uintptr_t)&nstate->wr_buff;
  nstate->read.length     = sizeof(tcp_dev_wr_t);
  nstate->read.done       = 0;
      
  nstate->write.wr_id     = 0;
  nstate->write.opcode    = SEND_DISABLED;
  nstate->write.cq_handle = CQ_HANDLE_NONE;
  nstate->write.addr      = (uintptr_t)NULL;
  nstate->write.length    = 0;
  nstate->write.done      = 0;
      
  struct epoll_event nev = 
    {
      .data.ptr = nstate,
      .events = EPOLLIN | EPOLLRDHUP
    };
      
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_sock, &nev) == -1)
    {
      close(conn_sock);
      free(nstate);
      return NULL;
    }

  return nstate;
}

/* make connection to all other nodes */
static int
_tcp_dev_connect_all(int epollfd)
{
  int i;

  /* TODO: glb_gaspi_ctx does not belong here */
  for(i = glb_gaspi_ctx.rank + 1; i < glb_gaspi_ctx.tnc; i++)
    {
      if((rank_state[i] != NULL))
	{
	  printf("TCP dev Rank %d: already connected to %d\n", glb_gaspi_ctx.rank, i);
	  continue;
	}

      /* connect to node/rank */
      /* TODO: use a generic sockets comm framework */
      int conn_sock = gaspi_connect2port(gaspi_get_hn(i),
				   TCP_DEV_PORT + glb_gaspi_ctx.poff[i],
				   CONN_TIMEOUT);

      if(conn_sock == -1)
	{
	  gaspi_dev_print_error("Error connecting to rank %i (%s) on port %i\n",
				i,
				gaspi_get_hn(i),
				TCP_DEV_PORT + glb_gaspi_ctx.poff[i]);
	  return 1;
	}
      
      /* prepare work request */
      tcp_dev_wr_t wr;
      memset(&wr, 0, sizeof(tcp_dev_wr_t));

      wr.wr_id       = glb_gaspi_ctx.tnc;
      wr.cq_handle   = CQ_HANDLE_NONE;
      wr.source      = glb_gaspi_ctx.rank;
      wr.local_addr  = (uintptr_t) NULL;
      wr.target      = i;
      wr.remote_addr = (uintptr_t) NULL;
      wr.length      = sizeof(tcp_dev_wr_t);

      /* TODO: glb_gaspi_ctx doesn't belong here*/
      if(glb_gaspi_ctx.rank == 0)
	  wr.opcode = REGISTER_MASTER;
      else
	  wr.opcode = REGISTER_WORKER;

      if(write(conn_sock, &wr, sizeof(tcp_dev_wr_t)) < 0)
	{
	  gaspi_dev_print_error("Failed to send registration request to rank %i (%s)\n",
				i, gaspi_get_hn(i));
	  close(conn_sock);
	  return 1;
	}

      /* add new socket to epoll instance */
      tcp_dev_conn_state_t *nstate;
      nstate = _tcp_dev_add_new_conn(i, conn_sock, epollfd);
      if( nstate == NULL)
	{
	  gaspi_dev_print_error("Failed to add connect to events instance");
	  return 1;
	}
      
      /* register rank */
      rank_state[i] = nstate;
    }  

  tcp_dev_connected_to_all = 1;
  
  return 0;
}

int
tcp_dev_stop_device(int fd)
{
  size_t done = 0;
  
  tcp_dev_wr_t wr;
  wr.wr_id       = glb_gaspi_ctx.rank;
  wr.cq_handle   = CQ_HANDLE_NONE;
  wr.source      = glb_gaspi_ctx.rank;
  wr.local_addr  = (uintptr_t) NULL;
  wr.target      = glb_gaspi_ctx.rank;
  wr.remote_addr = (uintptr_t) NULL;
  wr.length      = sizeof(tcp_dev_wr_t);
  
  wr.opcode = STOP_DEVICE;
  
  while(done < sizeof(tcp_dev_wr_t))
    {
      const int bytes_sent = write(fd, (char *) &wr + done, sizeof(tcp_dev_wr_t) - done);
      
      if(bytes_sent <= 0 && !(errno == EAGAIN || errno == EWOULDBLOCK))
	{
	  return 1;
	}
      else if( bytes_sent > 0)
	{
	  done += bytes_sent;
	}
    }

  return 0;
}

inline int
tcp_dev_return_wc(struct tcp_cq *cq, tcp_dev_wc_t *wc)
{
  void *ret;

  if(cq->rbuf == NULL)
    {
      gaspi_dev_print_error("Wrong completion queue\n");
      return -1;
    }
  
  if(remove_ringbuffer(cq->rbuf, &ret) < 0)
    return 0;

  *wc = *((tcp_dev_wc_t *) ret);

  return 1;
}

/* Post a work completion */
static inline int
_tcp_dev_post_wc(uint64_t wr_id,
		 enum tcp_dev_wc_status status,
		 enum tcp_dev_wc_opcode opcode,
		 uint32_t cq_handle)
{
  tcp_dev_wc_t* wc = (tcp_dev_wc_t *) malloc( sizeof(tcp_dev_wc_t) );
  if(wc == NULL)
    {
      gaspi_dev_print_error("Failed to allocate WC");
      return 1;
    }

  wc->wr_id  = wr_id;
  wc->status = status;
  wc->opcode = opcode;

  /* TODO: better approach when queue is full ? */
  while(insert_ringbuffer(cqs_map[cq_handle]->rbuf, wc) < 0)
    {
      printf("completion queue #%i is full.\n", cq_handle);
      __asm__ ( "pause;" );
    }

  /* acknowledge receiver (if that's the case) */
  if(opcode == TCP_DEV_WC_RECV)
    {
      char ping = 1;
      wc->sender = wr_id;
      
      if( write(cqs_map[cq_handle]->pchannel->write, &ping, 1) < 1)
	{
	  printf("Failed to write cq notification\n");
	  return 1;
	}
    }

  return 0;
}

static inline void
_tcp_dev_set_default_read_conn_state(tcp_dev_conn_state_t *estate)
{
  /* These are the defaults for almost all read opcodes */
  /* for the different cases these are updated as required */
  estate->read.wr_id     = 0;
  estate->read.cq_handle = CQ_HANDLE_NONE;
  estate->read.opcode    = RECV_HEADER;
  estate->read.addr      = (uintptr_t)&estate->wr_buff;
  estate->read.length    = sizeof(tcp_dev_wr_t);
  estate->read.done      = 0;
}

static int
_tcp_dev_process_recv_data(tcp_dev_conn_state_t *estate, int epollfd)
{
  enum tcp_dev_wc_opcode op;

  if(estate->read.opcode == RECV_HEADER)
    {
      switch(estate->wr_buff.opcode)
	{
	  /* TOPOLOGY OPERATIONS */
	case REGISTER_MASTER:

	  if(_tcp_dev_alloc_remote_states(glb_gaspi_ctx.tnc) != 0)
	    {
	      return 1;
	    }

	  estate->rank = 0;
	  rank_state[0] = estate;

	  if(_tcp_dev_connect_all(epollfd) != 0)
	    {
	      return 1;
	    }
	  
	  _tcp_dev_set_default_read_conn_state(estate);
	  
	  break;
	case REGISTER_WORKER:
	  if(_tcp_dev_alloc_remote_states(glb_gaspi_ctx.tnc) != 0)
	    {
	      return 1;
	    }

	  estate->rank = estate->wr_buff.source;
	  rank_state[estate->rank] = estate;

	  _tcp_dev_set_default_read_conn_state(estate);
	  
	  break;

	  /* RDMA OPERATIONS */
	case POST_RDMA_WRITE:
	case POST_RDMA_WRITE_INLINED:
	case POST_RDMA_READ:

	  if(estate->wr_buff.opcode == POST_RDMA_READ)
	    op = TCP_DEV_WC_RDMA_READ;
	  else
	    op = TCP_DEV_WC_RDMA_WRITE;
	  
	  /* local operation: do it right away */
	  if(estate->wr_buff.target == glb_gaspi_ctx.rank)
	    {
	      void *src;
	      void *dest;
	      if(estate->wr_buff.opcode == POST_RDMA_READ)
		{
		  src  = (void*)estate->wr_buff.remote_addr;
		  dest = (void*)estate->wr_buff.local_addr;
		}
	      else
		{
		  src =  (void *) estate->wr_buff.local_addr;
		  dest = (void *) estate->wr_buff.remote_addr;
		}
	      
	      memcpy(dest, src, estate->wr_buff.length);

	      if( _tcp_dev_post_wc(estate->wr_buff.wr_id, TCP_WC_SUCCESS, op, estate->wr_buff.cq_handle) != 0)
		{
		  return 1;
		}

	      /* release memory of inlined writes */
	      if(estate->wr_buff.opcode == POST_RDMA_WRITE_INLINED)
		{
		  free(src);
		}
	    }
	  else
	    {
	      tcp_dev_wr_t wr =
		{
		  .wr_id       = estate->wr_buff.wr_id,
		  .cq_handle   = estate->wr_buff.cq_handle,
		  .source      = estate->wr_buff.source,
		  .target      = estate->wr_buff.target,
		  .local_addr  = estate->wr_buff.local_addr,
		  .remote_addr = estate->wr_buff.remote_addr,
		  .length      = estate->wr_buff.length,
		  .swap        = 0
		} ;

	      if(estate->wr_buff.opcode == POST_RDMA_READ)
		{
		  wr.opcode      = REQUEST_RDMA_READ;
		  wr.compare_add = 0;
		}
	      else
		{
		  wr.opcode      = NOTIFICATION_RDMA_WRITE;
		  wr.compare_add = (estate->wr_buff.opcode == POST_RDMA_WRITE) ? 0 : 1; // indicates inlined
		}

	      //TODO: check retval
	      list_insert(&delayedList, &wr);
	    }

	  _tcp_dev_set_default_read_conn_state(estate);

	  break;
	  
	case POST_ATOMIC_CMP_AND_SWP:
	case POST_ATOMIC_FETCH_AND_ADD:

	  if(estate->wr_buff.opcode == POST_ATOMIC_FETCH_AND_ADD)
	    op = TCP_DEV_WC_FETCH_ADD;
	  else
	    op = TCP_DEV_WC_CMP_SWAP;

	  if(estate->wr_buff.target == glb_gaspi_ctx.rank)
	    {
	      uint64_t *ptr = (uint64_t *) estate->wr_buff.remote_addr;
	      uint64_t *dest = (uint64_t *) estate->wr_buff.local_addr;

	      /* return old value */
	      *dest = *ptr;

	      if(estate->wr_buff.opcode == POST_ATOMIC_CMP_AND_SWP)
		{
		  if(*ptr == estate->wr_buff.compare_add)
		    *ptr = estate->wr_buff.swap;
		}
	      
	      else if(estate->wr_buff.opcode == POST_ATOMIC_FETCH_AND_ADD)
		*ptr += estate->wr_buff.compare_add;

	      if( _tcp_dev_post_wc(estate->wr_buff.wr_id, TCP_WC_SUCCESS, op, estate->wr_buff.cq_handle) != 0)
		{
		  return 1;
		}
	    }
	  else
	    {
	      tcp_dev_wr_t wr =
		{
		  .wr_id       = estate->wr_buff.wr_id,
		  .cq_handle   = estate->wr_buff.cq_handle,
		  .source      = estate->wr_buff.source,
		  .target      = estate->wr_buff.target,
		  .local_addr  = estate->wr_buff.local_addr,
		  .remote_addr = estate->wr_buff.remote_addr,
		  .length      = estate->wr_buff.length,
		  .compare_add = estate->wr_buff.compare_add
		};
	      
	      if(op == TCP_DEV_WC_FETCH_ADD)
		{
		  wr.swap = 0;
		  wr.opcode = REQUEST_ATOMIC_FETCH_AND_ADD;
		}
	      else if(op == TCP_DEV_WC_CMP_SWAP)
		{
		  wr.swap = estate->wr_buff.swap;
		  wr.opcode = REQUEST_ATOMIC_CMP_AND_SWP;
		}

	      /* TODO: retval */
	      list_insert(&delayedList, &wr);
	    }
	  _tcp_dev_set_default_read_conn_state(estate);
	  
	  break;
	case POST_SEND:
	case POST_SEND_INLINED:
	  {
	  tcp_dev_wr_t wr =
	    {
	      .wr_id       = estate->wr_buff.wr_id,
	      .cq_handle   = estate->wr_buff.cq_handle,
	      .opcode      = NOTIFICATION_SEND,
	      .source      = estate->wr_buff.source,
	      .target      = estate->wr_buff.target,
	      .local_addr  = estate->wr_buff.local_addr,
	      .remote_addr = estate->wr_buff.remote_addr,
	      .length      = estate->wr_buff.length,
	      .compare_add = estate->wr_buff.compare_add
	    };

	  list_insert(&delayedList, &wr);
	  
	  _tcp_dev_set_default_read_conn_state(estate);
	  }
	  
	  break;
	case POST_RECV:
	  list_insert(&recvList, &(estate->wr_buff));
	  
	  _tcp_dev_set_default_read_conn_state(estate);
	  
	  break;
	  
	case NOTIFICATION_RDMA_WRITE:
	  estate->read.wr_id     = estate->wr_buff.wr_id;
	  estate->read.cq_handle = estate->wr_buff.cq_handle;
	  estate->read.opcode    = RECV_RDMA_WRITE;
	  estate->read.addr      = estate->wr_buff.remote_addr;
	  estate->read.length    = estate->wr_buff.length;
	  estate->read.done      = 0;
	  break;

	case REQUEST_RDMA_READ:
	  {
	    tcp_dev_wr_t wr =
	      {
		.wr_id       = estate->wr_buff.wr_id,
		.cq_handle   = estate->wr_buff.cq_handle,
		.opcode      = RESPONSE_RDMA_READ,
		.source      = estate->wr_buff.target,
		.target      = estate->wr_buff.source,
		.local_addr  = estate->wr_buff.remote_addr,
		.remote_addr = estate->wr_buff.local_addr,
		.length      = estate->wr_buff.length,
		.compare_add = estate->wr_buff.compare_add,
		.swap        = estate->wr_buff.swap
	      } ;
	    
	    list_insert(&delayedList, &wr);
	  }
	  _tcp_dev_set_default_read_conn_state(estate);

	  break;
	case RESPONSE_RDMA_READ:
	  
	  estate->read.wr_id     = estate->wr_buff.wr_id;
	  estate->read.cq_handle = estate->wr_buff.cq_handle;
	  estate->read.opcode    = RECV_RDMA_READ;
	  estate->read.addr      = estate->wr_buff.remote_addr;
	  estate->read.length    = estate->wr_buff.length;
	  estate->read.done      = 0;
	  break;
	  
	case REQUEST_ATOMIC_CMP_AND_SWP:
	case REQUEST_ATOMIC_FETCH_AND_ADD:
	  {
	    tcp_dev_wr_t wr =
	      {
		.wr_id       = estate->wr_buff.wr_id,
		.cq_handle   = estate->wr_buff.cq_handle,
		.opcode      = (estate->wr_buff.opcode == REQUEST_ATOMIC_CMP_AND_SWP) ? RESPONSE_ATOMIC_CMP_AND_SWP : RESPONSE_ATOMIC_FETCH_AND_ADD,
		.source      = estate->wr_buff.target,
		.target      = estate->wr_buff.source,
		.local_addr  = estate->wr_buff.remote_addr,
		.remote_addr = estate->wr_buff.local_addr,
		.length      = estate->wr_buff.length,
		.compare_add = estate->wr_buff.compare_add,
		.swap        = estate->wr_buff.swap
	      } ;

	    uint64_t *ptr = (uint64_t *) estate->wr_buff.remote_addr;
	    if(estate->wr_buff.opcode == REQUEST_ATOMIC_CMP_AND_SWP)
	      {
		const uint64_t old = *ptr;
		if(*ptr == estate->wr_buff.compare_add)
		  {
		    *ptr = estate->wr_buff.swap;
		  }
		wr.compare_add = old; 
	      }
	    else if(estate->wr_buff.opcode == REQUEST_ATOMIC_FETCH_AND_ADD)
	      {
		wr.compare_add = *ptr;
		*ptr += estate->wr_buff.compare_add;
	      }

	    list_insert(&delayedList, &wr);;
	  }
	  _tcp_dev_set_default_read_conn_state(estate);

	  break;
	  
	case RESPONSE_ATOMIC_CMP_AND_SWP:
	case RESPONSE_ATOMIC_FETCH_AND_ADD:
	  {
	    uint64_t *ptr = (uint64_t *) estate->wr_buff.remote_addr;
	    *ptr = estate->wr_buff.compare_add;
	  }
	  
	  if(estate->wr_buff.opcode == RESPONSE_ATOMIC_CMP_AND_SWP)
	    op = TCP_DEV_WC_CMP_SWAP;
	  else
	    op = TCP_DEV_WC_FETCH_ADD;

	  if(_tcp_dev_post_wc(estate->wr_buff.wr_id, TCP_WC_SUCCESS, op, estate->wr_buff.cq_handle))
	    {
	      return 1;
	    }
	  
	  _tcp_dev_set_default_read_conn_state(estate);
	  
	  break;
	case NOTIFICATION_SEND:
	  if(recvList.count)
	    {
	      tcp_dev_wr_t swr = estate->wr_buff;
	      tcp_dev_wr_t rwr = recvList.first->wr;

	      int found = 0;

	      listNode *to_remove = recvList.first;

	      while(to_remove != NULL)
		{
		  if(swr.length <= to_remove->wr.length)
		    {
		      rwr = to_remove->wr;
		      found = 1;
		      break;
		    }
		  to_remove = to_remove->next;
		}
	      if (!found)
		break;

	      list_remove(&recvList, to_remove);

	      tcp_dev_wr_t wr = 
		{
		  .wr_id       = swr.wr_id,
		  .cq_handle   = swr.cq_handle,
		  .opcode      = RESPONSE_SEND,
		  .source      = swr.target,
		  .target      = swr.source,
		  .local_addr  = swr.local_addr,
		  .remote_addr = swr.remote_addr,
		  .length      = swr.length,
		  .compare_add = swr.compare_add,
		  .swap        = swr.swap
		};
	      
	      list_insert(&delayedList, &wr);

	      estate->read.wr_id     = estate->rank;
	      estate->read.cq_handle = rwr.cq_handle;
	      estate->read.opcode    = RECV_SEND;
	      estate->read.addr      = (uintptr_t)rwr.local_addr;
	      estate->read.length    = swr.length;
	      estate->read.done      = 0;
	    }
	  
	  break;
	case RESPONSE_SEND:
	  if(_tcp_dev_post_wc(estate->wr_buff.wr_id, TCP_WC_SUCCESS, TCP_DEV_WC_SEND, estate->wr_buff.cq_handle) != 0)
	    {
	      return 1;
	    }

	  _tcp_dev_set_default_read_conn_state(estate);
	  
	  break;
	  /* OPERATIVE */
	case STOP_DEVICE:
	  return -1;
	  break;
	} /* switch opcode */
    } /* if RECV_HEADER*/

  else if(estate->read.opcode == RECV_RDMA_WRITE)
    {
      _tcp_dev_set_default_read_conn_state(estate);
    }

  else if(estate->read.opcode == RECV_RDMA_READ)
    {
      if(_tcp_dev_post_wc(estate->read.wr_id, TCP_WC_SUCCESS, TCP_DEV_WC_RDMA_READ,
			  estate->read.cq_handle) != 0)
	{
	  return 1;
	}

      _tcp_dev_set_default_read_conn_state(estate);
    }
  
  else if(estate->read.opcode == RECV_SEND)
    {
      if(_tcp_dev_post_wc(estate->read.wr_id, TCP_WC_SUCCESS, TCP_DEV_WC_RECV, estate->read.cq_handle))
	{
	  return 1;
	}
      
      _tcp_dev_set_default_read_conn_state(estate);
    }
  else
    {
      /* Unknown opcode */
      return 1;
    }
  
  return 0;
}

static int
_tcp_dev_process_sent_data(int epollfd, tcp_dev_conn_state_t *estate)
{
  if(estate->write.opcode == SEND_RDMA_WRITE)
    {
      if(_tcp_dev_post_wc(estate->write.wr_id, TCP_WC_SUCCESS, TCP_DEV_WC_RDMA_WRITE, estate->write.cq_handle) != 0)
	{
	  return 1;
	}
    }

  struct epoll_event ev = 
    {
      .data.ptr = estate,
      .events = EPOLLIN | EPOLLRDHUP
    };

  if(epoll_ctl(epollfd, EPOLL_CTL_MOD, estate->fd, &ev) < 0)
    {
      gaspi_dev_print_error("Failed to modify events instance.");
      return 1;
    }
  
  estate->write.wr_id     = 0;
  estate->write.cq_handle = CQ_HANDLE_NONE;
  estate->write.opcode    = SEND_DISABLED;
  estate->write.addr      = (uintptr_t) NULL;
  estate->write.length    = 0;
  estate->write.done      = 0;
  
  return 0;
}

static int
_tcp_dev_process_delayed(int epollfd)
{

  if(delayedList.count == 0 || rank_state == NULL)
    return 0;

  listNode * element = delayedList.first;
  listNode *delete = NULL;

  while(element != NULL)
    {
      tcp_dev_conn_state_t *state = rank_state[element->wr.target];
      tcp_dev_wr_t wr = element->wr;

      if(state == NULL && !(wr.opcode == NOTIFICATION_SEND && wr.target == glb_gaspi_ctx.rank))
	{
	  if( _tcp_dev_post_wc(wr.wr_id, TCP_WC_REM_OP_ERROR, TCP_DEV_WC_RDMA_WRITE, wr.cq_handle) != 0)
	    {
	      gaspi_dev_print_error("Failed to post completion error.");
	      return 1;
	    }

	  delete = element;
	}
      else if(wr.opcode == NOTIFICATION_SEND && (wr.target == glb_gaspi_ctx.rank))
	{
	  if(recvList.count)
	    {
	      tcp_dev_wr_t rwr = recvList.first->wr;

	      if(rwr.length < wr.length)
		{
		  gaspi_dev_print_error("Size mismath between work requests.");
		  return 1;
		}
	      list_remove(&recvList, recvList.first);

	      void *src = (void *) wr.local_addr;
	      void *dest = (void *) rwr.local_addr;

	      memcpy(dest, src, wr.length);

	      struct tcp_cq *cq = cqs_map[rwr.cq_handle];
	      
	      if(_tcp_dev_post_wc(wr.wr_id, TCP_WC_SUCCESS, TCP_DEV_WC_SEND, wr.cq_handle) != 0)
		{
		  return 1;
		}
	      if(_tcp_dev_post_wc(rwr.wr_id, TCP_WC_SUCCESS, TCP_DEV_WC_RECV, (cq != NULL) ? cq->num : CQ_HANDLE_NONE) != 0)
		{
		  return 1;
		}

	      /* release memory of inlined writes */
	      if(wr.compare_add == 1)
		free(src);

	      delete = element;
	    }
	}
      else if(state->write.opcode == SEND_DISABLED)
	{
	  size_t done = 0;
	  while(done < sizeof(tcp_dev_wr_t))
	    {
	      const int bytes_sent = write(state->fd, (char *) &wr + done, sizeof(tcp_dev_wr_t) - done);

	      if(bytes_sent <= 0 && !(errno == EAGAIN || errno == EWOULDBLOCK))
		{
		  //TODO: better handling
		  close(state->fd);
		  return 1;
		}
	      else if( bytes_sent > 0)
		{
		  done += bytes_sent;
		}
	    }
	  if( (wr.opcode == NOTIFICATION_RDMA_WRITE && wr.compare_add == 1)
	      || (wr.opcode == NOTIFICATION_SEND && wr.compare_add == 1))
	    {
	      ssize_t done = 0;
	      
	      while(done < element->wr.length)
		{
		  const int bytes_sent = write(state->fd, (void *)element->wr.local_addr + done, element->wr.length - done);
		  
		  if(bytes_sent <= 0 && !(errno == EAGAIN || errno == EWOULDBLOCK))
		    {
		      //TODO: better handling
		      printf("Rank %d: failed to write on %d\n", glb_gaspi_ctx.rank, state->fd);
		     
		      close(state->fd);
		      return 1;
		    }
		  else if( bytes_sent > 0)
		    {
		      done += bytes_sent;
		    }
		}
	      
	      enum tcp_dev_wc_opcode op;

	      wr.opcode == NOTIFICATION_RDMA_WRITE ? op = TCP_DEV_WC_RDMA_WRITE : TCP_DEV_WC_SEND;
	      if(_tcp_dev_post_wc(element->wr.wr_id, TCP_WC_SUCCESS, TCP_DEV_WC_RDMA_WRITE, element->wr.cq_handle) != 0)
		{
		  gaspi_dev_print_error("Failed to post success.");
		  return 1;
		}
	      
	      free((void *) element->wr.local_addr);
	    }
	  else if(   wr.opcode == NOTIFICATION_RDMA_WRITE
		  || wr.opcode == RESPONSE_RDMA_READ
		  || wr.opcode == NOTIFICATION_SEND )
	    {
	      /* enable write notification */
	      if(wr.opcode == NOTIFICATION_RDMA_WRITE)
		state->write.opcode = SEND_RDMA_WRITE;
	      else if(wr.opcode == RESPONSE_RDMA_READ)
		state->write.opcode = SEND_RDMA_READ;
	      else
		state->write.opcode = SEND_SEND;
	      
	      state->write.wr_id     = wr.wr_id;
	      state->write.cq_handle = wr.cq_handle;
	      state->write.addr      = wr.local_addr; 
	      state->write.length    = wr.length;
	      state->write.done      = 0;
	      
	      struct epoll_event ev = {
		.data.ptr = state,
		.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP
	      };

	      if (epoll_ctl(epollfd, EPOLL_CTL_MOD, state->fd, &ev) < 0)
		{
		  gaspi_dev_print_error("Failed to modify events instance.");
		  close(state->fd);
		  exit(EXIT_FAILURE);
		}
	    }
	  delete = element;
	}
      
      element = element->next;
      if(delete)
	{
	  list_remove(&delayedList, delete);
	  delete = NULL;
	}
    }
  
  return 0;
}

/* virtual device thread body */
void *
tcp_virt_dev(void *args)
{
  volatile int valid_state = 1; /* flag indicates state health */
  
  int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if(listen_sock < 0)
    {
      gaspi_dev_print_error("Failed to create socket");
      return NULL;
    }

  int opt = 1;
  if(setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <  0)
    {
      gaspi_dev_print_error("Failed to modify socket");
      return NULL;
    }

  if(setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0)
    {
      gaspi_dev_print_error("Failed to modify socket");
      return NULL;
    }
  
  signal(SIGPIPE, SIG_IGN);

  struct sockaddr_in listenAddr =
    {
      .sin_family = AF_INET,
      .sin_port = htons (TCP_DEV_PORT + glb_gaspi_ctx.localSocket), /* TODO: glb_gaspi_ctx does not belong here*/
      .sin_addr.s_addr = htonl(INADDR_ANY)
    };

  if(bind(listen_sock, (struct sockaddr *) (&listenAddr), sizeof(listenAddr)) < 0)
    {
      gaspi_dev_print_error("Failed to bind to port %d\n", TCP_DEV_PORT + glb_gaspi_ctx.localSocket); /* TODO: glb_gaspi_ctx does not belong here*/
      return NULL;
    }
  
  gaspi_set_non_blocking(listen_sock);

  if(listen(listen_sock, SOMAXCONN) < 0)
    {
      gaspi_dev_print_error("Failed to listen on socket");
      return NULL;
    }
    
  int epollfd = epoll_create(MAX_EVENTS);
  if(epollfd == -1)
    {
      gaspi_dev_print_error("Failed to create epoll instance");
      return NULL;
    }

  tcp_dev_conn_state_t *lstate = malloc(sizeof(tcp_dev_conn_state_t));
  if( lstate == NULL)
    {
      gaspi_dev_print_error("Failed to allocate memory");
      return NULL;
    }

  lstate->fd = listen_sock;
  lstate->rank = -1;

  struct epoll_event lev =
    {
      .data.ptr = lstate,
      .events = EPOLLIN | EPOLLRDHUP
    };
  
  if(epoll_ctl(epollfd, EPOLL_CTL_ADD, listen_sock, &lev) < 0)
    {
      gaspi_dev_print_error("Failed to add socket to event instance.");
      return NULL;
    }

  /* TODO: gaspi_ctx has nothing to do here */
  if(glb_gaspi_ctx.rank == 0)
    {
      if( _tcp_dev_alloc_remote_states (glb_gaspi_ctx.tnc) != 0)
	{
	  return NULL;
	}

      if(_tcp_dev_connect_all(epollfd) != 0)
	{
	  return NULL;
	}
    }

  /* events loop */
  struct epoll_event *events = calloc(MAX_EVENTS, sizeof(struct epoll_event));
  if(events == NULL)
    {
      gaspi_dev_print_error("Failed to allocate events buffer");
      return NULL;
    }

  while(valid_state)
    {
      int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
      if(nfds < 0)
	{
	  gaspi_dev_print_error("Failed to wait event. Device is unstable");
	  //exit?
	}

      int n;
      for(n = 0; valid_state && (n < nfds); ++n)
	{
	  tcp_dev_conn_state_t *estate = (tcp_dev_conn_state_t *)events[n].data.ptr;
	  const int event_fd = estate->fd;
	  const int event_rank = estate->rank;
	  int io_err = 0;

	  if(events[n].events & EPOLLERR || events[n].events & EPOLLHUP || events[n].events & EPOLLRDHUP)
	    {
	      io_err = 1;
	    }
	  
	  /* new incoming connection */
	  else if(event_fd == listen_sock)
	    {
	      while(1)
		{
		  struct sockaddr local;
		  socklen_t addrlen = sizeof(local);

		  int conn_sock = accept(listen_sock, (struct sockaddr *) &local, &addrlen);
		  if(conn_sock < 0)
		    {
		      if(errno == EAGAIN || errno == EWOULDBLOCK)
			break; 

		      gaspi_dev_print_error("Failed to accept connection");
		      continue;
		    }

		  gaspi_set_non_blocking(conn_sock);

		  /* TODO: meaningful number: -1 = UNKNOWN_RANK */
		  if(_tcp_dev_add_new_conn(-1, conn_sock, epollfd) == NULL)
		    {
		      gaspi_dev_print_error("Failed to add to events instance");
		    }
		}
	      continue;
	    }
	  else /* IO ops*/
	    {
	      if(events[n].events & EPOLLIN)
		{
		  /* read until would block */
		  while(1) 
		    {		      
		      // TODO: catch NULL-ptr
		      const int bytesRemaining = estate->read.length - estate->read.done;
		      const int bytesReceived  = read(estate->fd, (void*) estate->read.addr + estate->read.done, bytesRemaining);
		      
		      /* would block */
		      if(bytesReceived < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
			  break;
			}
		      else if(bytesReceived == 0 && bytesRemaining == 0)
			{
			  //			  gaspi_dev_print_error("Error rea node %d", event_rank);
			  // Recv WR not present
			}
		      else if(bytesReceived <= 0)
			{
			  gaspi_dev_print_error("Error reading from node %d", event_rank);
			  io_err = 1;
			  break;
			}
		      // success
		      else
			{
			  estate->read.done += bytesReceived;
			}
		      
		      if(estate->read.done == estate->read.length)
			{
			  int ret = _tcp_dev_process_recv_data(estate, epollfd);
			  
			  if( ret != 0)
			    {
			      /* stop device */
			      if(ret < 0)
				goto tcp_virt_dev_cleanup;
			      
			      printf("virt_device: failed to process received data\n");
			      //TODO: better error? exit?
			    }
			  break;
			}
		    }
		}
	      /* write data */
	      if(!io_err && (events[n].events & EPOLLOUT))
		{
		  /* write until would block */
		  while(1)
		    {
		      // TODO: catch NULL-ptr
		      const int bytesRemaining = estate->write.length - estate->write.done;
		      const int bytesSent      = write(estate->fd, (void*)estate->write.addr + estate->write.done, bytesRemaining);

		      // would block
		      if(bytesSent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			{
			  break;
			}
		      else if(bytesSent <= 0)
			{
			  gaspi_dev_print_error("Failed to write");
			  io_err = 1;
			  break;
			}
		      else
			{
			  estate->write.done += bytesSent;
			}

		      /*  data transfer is complete */
		      if(estate->write.done == estate->write.length)
			{
			  if(_tcp_dev_process_sent_data(epollfd, estate) != 0)
			    {
			      gaspi_dev_print_error("Failed to process sent data");
			      //TODO: better error handling?
			    }
			  break;
			}
		    }
		} /* write data */
	    } /* actual I/O */

	  /* we found an error? */
	  if(io_err)
	    {
	      /* remove socket from epoll instance */
	      struct epoll_event ev;
	      epoll_ctl(epollfd, EPOLL_CTL_DEL, event_fd, &ev);

	      /* actual error on a fd */
	      if( !(events[n].events & EPOLLRDHUP) )
		{
		  int error = 0;
		  socklen_t errlen = sizeof(error);
		  getsockopt(event_fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen);
		}

	      /* remote peer closed connection */
	      else if(event_rank >= 0)
		{
		  // TODO: post WC for all open WRs (for those in delayed list will be done in processdelayed())
		  rank_state[event_rank] = NULL;
		}

	      close(event_fd);
	      if(estate != NULL)
		free(estate);
	    } 
	} /* for all triggered events */

      /* handle delayed operations */
      if(_tcp_dev_process_delayed(epollfd) > 0)
	{
	  printf("virt_device: failed to process delayed events\n");
	}
    } /* device event loop */

 tcp_virt_dev_cleanup:
  {
    int k;

    /* TODO: glb_gaspi_ctx does not belong here */
    for(k = 0; k < glb_gaspi_ctx.tnc; k++)
      {
	if(!rank_state || !rank_state[k])
	  continue;

	struct epoll_event ev;
	epoll_ctl(epollfd, EPOLL_CTL_DEL, rank_state[k]->fd, &ev);
	close(rank_state[k]->fd);

	free(rank_state[k]);
	rank_state[k] = NULL;
      }

    if(rank_state)
      {
	free(rank_state);
	rank_state = NULL;
      }

    if(events)
      {
	free(events);
	events = NULL;
      }

    list_clear(&delayedList);

    close(epollfd);

    close(listen_sock);
  }
  
  return NULL;
}
