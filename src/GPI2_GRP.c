/*
Copyright (c) Fraunhofer ITWM - Carsten Lojewski <lojewski@itwm.fhg.de>, 2013-2016

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
#include <stdint.h>
#include <sys/timeb.h>
#include <sys/mman.h>
#include <unistd.h>

#include "PGASPI.h"
#include "GPI2.h"
#include "GPI2_Coll.h"
#include "GPI2_Dev.h"
#include "GPI2_GRP.h"
#include "GPI2_SN.h"
#include "GPI2_Utility.h"

#define GPI2_ALLREDUCE_ELEM_MAX ((1 << 8) - 1)
const unsigned int glb_gaspi_typ_size[6] = { 4, 4, 4, 8, 8, 8 };

static inline gaspi_return_t
_gaspi_release_group_mem(gaspi_context_t const * const gctx,
			 const gaspi_group_t group)
{
  //  gaspi_context_t const * const gctx = &glb_gaspi_ctx;
  gaspi_group_ctx_t * const grp_ctx = &(glb_gaspi_group_ctx[group]);

  if( grp_ctx->rrcd != NULL )
    {
      if( pgaspi_dev_unregister_mem(&(grp_ctx->rrcd[gctx->rank]))!= GASPI_SUCCESS )
	{
	  return GASPI_ERR_DEVICE;
	}

      free(grp_ctx->rrcd[gctx->rank].data.ptr);
      grp_ctx->rrcd[gctx->rank].data.ptr = NULL;

      free(grp_ctx->rrcd);
      grp_ctx->rrcd = NULL;
    }

  free(grp_ctx->rank_grp);
  grp_ctx->rank_grp = NULL;

  free(grp_ctx->committed_rank);
  grp_ctx->committed_rank = NULL;

  return GASPI_SUCCESS;
}

/* Group utilities */
#pragma weak gaspi_group_create = pgaspi_group_create
gaspi_return_t
pgaspi_group_create (gaspi_group_t * const group)
{
  int i, id = GASPI_MAX_GROUPS;
  const size_t size = NEXT_OFFSET;
  long page_size;
  gaspi_return_t eret = GASPI_ERROR;
  gaspi_context_t * const gctx = &glb_gaspi_ctx;

  gaspi_verify_init("gaspi_group_create");
  gaspi_verify_null_ptr(group);

  lock_gaspi_tout (&glb_gaspi_ctx_lock, GASPI_BLOCK);

  if( gctx->group_cnt >= GASPI_MAX_GROUPS )
    {
      unlock_gaspi (&glb_gaspi_ctx_lock);
      return GASPI_ERR_MANY_GRP;
    }

  for(i = 0; i < GASPI_MAX_GROUPS; i++)
    {
      if( glb_gaspi_group_ctx[i].id == -1 )
	{
	  id = i;
	  break;
	}
    }

  if( id == GASPI_MAX_GROUPS )
    {
      unlock_gaspi (&glb_gaspi_ctx_lock);
      return GASPI_ERR_MANY_GRP;
    }

  page_size = sysconf(_SC_PAGESIZE);

  if( page_size < 0 )
    {
      gaspi_print_error ("Failed to get system's page size.");
      goto errL;
    }

  GASPI_RESET_GROUP(glb_gaspi_group_ctx, id);

  gaspi_group_ctx_t * const new_grp_ctx = &(glb_gaspi_group_ctx[id]);

  new_grp_ctx->gl.lock = 0;
  new_grp_ctx->del.lock = 0;

  /* TODO: dynamic space (re-)allocation to avoid reservation for all nodes */
  /* or maybe gaspi_group_create should have the number of ranks as input ? */
  new_grp_ctx->rrcd = (gaspi_rc_mseg_t *) calloc ((size_t) gctx->tnc, sizeof (gaspi_rc_mseg_t));
  if( new_grp_ctx->rrcd == NULL )
    {
      eret = GASPI_ERR_MEMALLOC;
      goto errL;
    }

  if( posix_memalign ((void **) &(new_grp_ctx->rrcd[gctx->rank].data.ptr), (size_t) page_size, size) != 0 )
    {
      eret = GASPI_ERR_MEMALLOC;
      goto errL;
    }

  memset(new_grp_ctx->rrcd[gctx->rank].data.buf, 0, size);

  new_grp_ctx->rrcd[gctx->rank].size = size;

  eret = pgaspi_dev_register_mem(&(new_grp_ctx->rrcd[gctx->rank]));
  if( eret != GASPI_SUCCESS )
    {
      eret = GASPI_ERR_DEVICE;
      goto errL;
    }

  /* TODO: as above, more dynamic allocation */
  new_grp_ctx->rank_grp = (int *) malloc(gctx->tnc * sizeof (int));
  if( new_grp_ctx->rank_grp == NULL )
    {
      eret = GASPI_ERR_MEMALLOC;
      goto errL;
    }

  new_grp_ctx->committed_rank = (int *) calloc(gctx->tnc, sizeof (int));
  if( new_grp_ctx->committed_rank == NULL )
    {
      eret = GASPI_ERR_MEMALLOC;
      goto errL;
    }

  for(i = 0; i < gctx->tnc; i++)
    {
      new_grp_ctx->rank_grp[i] = -1;
    }

  gctx->group_cnt++;
  *group = id;

  new_grp_ctx->id = id;

  unlock_gaspi (&glb_gaspi_ctx_lock);
  return GASPI_SUCCESS;

 errL:
  _gaspi_release_group_mem(gctx, id);
  unlock_gaspi (&glb_gaspi_ctx_lock);

  return eret;
}


#pragma weak gaspi_group_delete = pgaspi_group_delete
gaspi_return_t
pgaspi_group_delete (const gaspi_group_t group)
{
  gaspi_verify_init("gaspi_group_delete");
  gaspi_verify_group(group);

  gaspi_context_t * const gctx = &glb_gaspi_ctx;
  gaspi_group_ctx_t * const grp_ctx = &(glb_gaspi_group_ctx[group]);

  gaspi_return_t eret = GASPI_ERROR;

  if( group == GASPI_GROUP_ALL )
    {
      return GASPI_ERR_INV_GROUP;
    }

  lock_gaspi (&(grp_ctx->del));

  eret = _gaspi_release_group_mem(gctx, group);

  GASPI_RESET_GROUP(glb_gaspi_group_ctx, group);

  unlock_gaspi (&(grp_ctx->del));

  lock_gaspi (&glb_gaspi_ctx_lock);

  gctx->group_cnt--;

  unlock_gaspi (&glb_gaspi_ctx_lock);

  return eret;
}

static int
gaspi_comp_ranks (const void *a, const void *b)
{
  return (*(int *) a - *(int *) b);
}

#pragma weak gaspi_group_add = pgaspi_group_add
gaspi_return_t
pgaspi_group_add (const gaspi_group_t group, const gaspi_rank_t rank)
{
  gaspi_verify_init("gaspi_group_add");
  gaspi_verify_rank(rank);
  gaspi_verify_group(group);

  gaspi_group_ctx_t * const grp_ctx = &(glb_gaspi_group_ctx[group]);

  lock_gaspi_tout (&glb_gaspi_ctx_lock, GASPI_BLOCK);

  int i;
  for(i = 0; i < grp_ctx->tnc; i++)
    {
      if( grp_ctx->rank_grp[i] == rank )
	{
	  unlock_gaspi (&glb_gaspi_ctx_lock);
	  return GASPI_ERR_INV_RANK;
	}
    }

  grp_ctx->rank_grp[grp_ctx->tnc++] = rank;

  qsort(grp_ctx->rank_grp, grp_ctx->tnc, sizeof (int), gaspi_comp_ranks);

  unlock_gaspi (&glb_gaspi_ctx_lock);
  return GASPI_SUCCESS;
}

static inline gaspi_return_t
_pgaspi_group_commit_to(const gaspi_group_t group,
			const gaspi_rank_t i,
			const gaspi_timeout_t timeout_ms)
{
  gaspi_return_t  eret = GASPI_ERROR;

  eret = gaspi_sn_command(GASPI_SN_GRP_CONNECT, i, timeout_ms, (void *) &group);
  if( eret != GASPI_SUCCESS )
    {
      return eret;
    }

  glb_gaspi_group_ctx[group].committed_rank[i] = 1;

  return GASPI_SUCCESS;
}

/* Internal shortcut for GASPI_GROUP_ALL */
/* Because we know the GROUP_ALL, we avoid checks, initial remote
   group check and connection. Overall try to do the minimum, mostly
   to speed-up initialization. */
gaspi_return_t
pgaspi_group_all_local_create(gaspi_context_t const * const gctx,
			      const gaspi_timeout_t timeout_ms)
{
  int i;
  gaspi_group_t g0;
  gaspi_return_t eret = GASPI_ERROR;

  if( (eret = pgaspi_group_create(&g0)) != GASPI_SUCCESS )
    {
      return eret;
    }

  if( g0 != GASPI_GROUP_ALL )
    {
      return GASPI_ERR_INV_GROUP;
    }

  if( lock_gaspi_tout (&glb_gaspi_ctx_lock, timeout_ms) )
    {
      return GASPI_TIMEOUT;
    }

  gaspi_group_ctx_t* const grp_all_ctx = &(glb_gaspi_group_ctx[GASPI_GROUP_ALL]);

  /* Add all ranks to it */
  for(i = 0; i < gctx->tnc; i++)
    {
      grp_all_ctx->rank_grp[i] = (gaspi_rank_t) i;
    }

  grp_all_ctx->tnc = gctx->tnc;


  grp_all_ctx->rank = gctx->rank;

  grp_all_ctx->next_pof2 = 1;
  while( grp_all_ctx->next_pof2 <= grp_all_ctx->tnc )
    {
      grp_all_ctx->next_pof2 <<= 1;
    }

  grp_all_ctx->next_pof2 >>= 1;
  grp_all_ctx->pof2_exp = (__builtin_clz (grp_all_ctx->next_pof2) ^ 31U);

  unlock_gaspi (&glb_gaspi_ctx_lock);
  return GASPI_SUCCESS;
}

gaspi_return_t
pgaspi_group_all_delete(gaspi_context_t * const gctx)
{
  gaspi_verify_init("gaspi_group_all_delete");

  gaspi_return_t eret = GASPI_ERROR;

  lock_gaspi_tout (&glb_gaspi_group_ctx[GASPI_GROUP_ALL].del, GASPI_BLOCK);

  eret = _gaspi_release_group_mem(gctx, GASPI_GROUP_ALL);

  GASPI_RESET_GROUP(glb_gaspi_group_ctx, GASPI_GROUP_ALL);

  unlock_gaspi (&glb_gaspi_group_ctx[GASPI_GROUP_ALL].del);

  lock_gaspi (&glb_gaspi_ctx_lock);

  gctx->group_cnt--;

  unlock_gaspi (&glb_gaspi_ctx_lock);

  return eret;
}

#pragma weak gaspi_group_commit = pgaspi_group_commit
gaspi_return_t
pgaspi_group_commit (const gaspi_group_t group,
		     const gaspi_timeout_t timeout_ms)
{
  int i, r;
  gaspi_return_t eret = GASPI_ERROR;
  gaspi_timeout_t delta_tout = timeout_ms;
  gaspi_context_t const * const gctx = &glb_gaspi_ctx;

  gaspi_verify_init("gaspi_group_commit");
  gaspi_verify_group(group);

  gaspi_group_ctx_t* group_to_commit = &(glb_gaspi_group_ctx[group]);

  if( lock_gaspi_tout (&glb_gaspi_ctx_lock, timeout_ms) )
    {
      return GASPI_TIMEOUT;
    }

  if( group_to_commit->tnc < 2 && gctx->tnc != 1 )
    {
      gaspi_print_error("Group must have at least 2 ranks to be committed");
      eret = GASPI_ERR_INV_GROUP;
      goto endL;
    }

  group_to_commit->rank = -1;

  for(i = 0; i < group_to_commit->tnc; i++)
    {
      if( group_to_commit->rank_grp[i] == gctx->rank )
	{
	  group_to_commit->rank = i;
	  break;
	}
    }

  if( group_to_commit->rank == -1 )
    {
      eret = GASPI_ERR_INV_GROUP;
      goto endL;
    }

  group_to_commit->next_pof2 = 1;

  while(group_to_commit->next_pof2 <= group_to_commit->tnc)
    {
      group_to_commit->next_pof2 <<= 1;
    }

  group_to_commit->next_pof2 >>= 1;
  group_to_commit->pof2_exp = (__builtin_clz (group_to_commit->next_pof2) ^ 31U);

  struct
  {
    gaspi_group_t group;
    int tnc, cs, ret;
  } gb;

  gb.group = group;
  gb.cs = 0;
  gb.tnc = group_to_commit->tnc;

  for(i = 0; i < group_to_commit->tnc; i++)
    {
      gb.cs ^= group_to_commit->rank_grp[i];
    }

  for(r = 1; r <= gb.tnc; r++)
    {
      int rg = (group_to_commit->rank + r) % gb.tnc;

      if(group_to_commit->rank_grp[rg] == gctx->rank)
	{
	  continue;
	}

      eret = gaspi_sn_command(GASPI_SN_GRP_CHECK, group_to_commit->rank_grp[rg], delta_tout, (void *) &gb);
      if( eret != GASPI_SUCCESS )
	{
	  goto endL;
	}

      if( _pgaspi_group_commit_to(group, group_to_commit->rank_grp[rg], timeout_ms) != 0 )
	{
	  gaspi_print_error("Failed to commit to %d", group_to_commit->rank_grp[rg]);
	  eret = GASPI_ERROR;
	  goto endL;
	}
    }

  eret = GASPI_SUCCESS;

 endL:
  unlock_gaspi (&glb_gaspi_ctx_lock);
  return eret;
}

#pragma weak gaspi_group_num = pgaspi_group_num
gaspi_return_t
pgaspi_group_num (gaspi_number_t * const group_num)
{
  gaspi_verify_init("gaspi_group_num");
  gaspi_verify_null_ptr(group_num);
  gaspi_context_t const * const gctx = &glb_gaspi_ctx;

  *group_num = gctx->group_cnt;

  return GASPI_SUCCESS;
}

#pragma weak gaspi_group_size = pgaspi_group_size
gaspi_return_t
pgaspi_group_size (const gaspi_group_t group,
		  gaspi_number_t * const group_size)
{
  gaspi_verify_init("gaspi_group_size");
  gaspi_context_t const * const gctx = &glb_gaspi_ctx;

  if( group < gctx->group_cnt )
    {
      gaspi_verify_null_ptr(group_size);

      *group_size = glb_gaspi_group_ctx[group].tnc;

      return GASPI_SUCCESS;
    }

  return GASPI_ERR_INV_GROUP;
}

#pragma weak gaspi_group_ranks = pgaspi_group_ranks
gaspi_return_t
pgaspi_group_ranks (const gaspi_group_t group,
		   gaspi_rank_t * const group_ranks)
{
  gaspi_verify_init("gaspi_group_ranks");
  gaspi_context_t const * const gctx = &glb_gaspi_ctx;

  if( group < gctx->group_cnt )
    {
      int i;
      for(i = 0; i < glb_gaspi_group_ctx[group].tnc; i++)
	{
	  group_ranks[i] = glb_gaspi_group_ctx[group].rank_grp[i];
	}

      return GASPI_SUCCESS;
    }

  return GASPI_ERR_INV_GROUP;
}

#pragma weak gaspi_group_max = pgaspi_group_max
gaspi_return_t
pgaspi_group_max (gaspi_number_t * const group_max)
{
  gaspi_verify_null_ptr(group_max);

  *group_max = GASPI_MAX_GROUPS;

  return GASPI_SUCCESS;
}

#pragma weak gaspi_allreduce_buf_size = pgaspi_allreduce_buf_size
gaspi_return_t
pgaspi_allreduce_buf_size (gaspi_size_t * const buf_size)
{
  gaspi_verify_null_ptr(buf_size);

  *buf_size = GPI2_REDUX_BUF_SIZE;

  return GASPI_SUCCESS;
}

#pragma weak gaspi_allreduce_elem_max = pgaspi_allreduce_elem_max
gaspi_return_t
pgaspi_allreduce_elem_max (gaspi_number_t * const elem_max)
{
  gaspi_verify_null_ptr(elem_max);

  *elem_max = GPI2_ALLREDUCE_ELEM_MAX;

  return GASPI_SUCCESS;
}

/* Group collectives */

/* Poll on location for expected sync value */
static inline gaspi_return_t
_gaspi_sync_wait(gaspi_context_t * const gctx,
		 volatile unsigned char* poll_buf,
		 unsigned char expected_val,
		 gaspi_timeout_t timeout_ms)
{
  const gaspi_cycles_t s0 = gaspi_get_cycles();

  while( *poll_buf != expected_val )
    {
      const gaspi_cycles_t s1 = gaspi_get_cycles();
      const gaspi_cycles_t tdelta = s1 - s0;
      const float ms = (float) tdelta * gctx->cycles_to_msecs;

      if( ms > timeout_ms )
	{
	  return GASPI_TIMEOUT;
	}
    }

  return GASPI_SUCCESS;
}

#define TOGGLE_SIZE 2
#define GPI2_GRP_LOCAL_SYNC_ADDR(grp_ctx) (grp_ctx->rrcd[glb_gaspi_ctx.rank].data.buf + (TOGGLE_SIZE * grp_ctx->tnc + grp_ctx->togle))
#define GPI2_GRP_REMOTE_SYNC_ADDR(grp_ctx, dst_rank) (grp_ctx->rrcd[dst_rank].data.addr + (TOGGLE_SIZE * grp_ctx->rank + grp_ctx->togle))
#define GPI2_GRP_SYNC_POLL_ADDR(grp_ctx, src_rank) (grp_ctx->rrcd[gctx->rank].data.buf + (TOGGLE_SIZE * src_rank + grp_ctx->togle))

#pragma weak gaspi_barrier = pgaspi_barrier
gaspi_return_t
pgaspi_barrier (const gaspi_group_t g, const gaspi_timeout_t timeout_ms)
{
  gaspi_verify_init("gaspi_barrier");
  gaspi_verify_group(g);

  gaspi_context_t * const gctx = &glb_gaspi_ctx;
  gaspi_group_ctx_t * const grp_ctx = &(glb_gaspi_group_ctx[g]);

  GPI2_STATS_START_TIMER(GASPI_BARRIER_TIMER);

  if( lock_gaspi_tout (&(grp_ctx->gl), timeout_ms) )
    {
      return GASPI_TIMEOUT;
    }

  if( !(grp_ctx->coll_op & GASPI_BARRIER) )
    {
      unlock_gaspi (&grp_ctx->gl);
      return GASPI_ERR_ACTIVE_COLL;
    }

  grp_ctx->coll_op = GASPI_BARRIER;

  if( grp_ctx->lastmask == 0x1 )
    {
      grp_ctx->barrier_cnt++;

      /* We need to take care of the wraparound of the barrier_cnt. By
	 increasing the counter we avoid seeing the same counter value
	 in the same togle position. */
      if( grp_ctx->barrier_cnt == 0 )
	{
	  grp_ctx->barrier_cnt++;
	}
    }

  const int grp_size = grp_ctx->tnc;
  const int rank_in_grp = grp_ctx->rank;

  unsigned char* const barrier_ptr = GPI2_GRP_LOCAL_SYNC_ADDR(grp_ctx);
  barrier_ptr[0] = grp_ctx->barrier_cnt;

  int mask = grp_ctx->lastmask & 0x7fffffff;
  int jmp = grp_ctx->lastmask >> 31;

  gaspi_return_t eret = GASPI_ERROR;

  while( mask < grp_size )
    {
      const int dst = grp_ctx->rank_grp[(rank_in_grp + mask) % grp_size];
      const int src = (rank_in_grp - mask + grp_size) % grp_size;

      if( jmp )
	{
	  jmp = 0;
	  goto B0;
	}

      if( GASPI_ENDPOINT_DISCONNECTED == gctx->ep_conn[dst].cstat )
	{
	  if( ( eret = pgaspi_connect((gaspi_rank_t) dst, timeout_ms)) != GASPI_SUCCESS )
	    {
	      gaspi_print_error("Failed to connect to rank %u", dst);
	      unlock_gaspi (&grp_ctx->gl);
	      return eret;
	    }
	}

      if( !grp_ctx->committed_rank[dst] )
	{
	  if( ( eret = _pgaspi_group_commit_to(g, dst, timeout_ms)) != GASPI_SUCCESS )
	    {
	      gaspi_print_error("Failed to commit to rank %u", dst);
	      unlock_gaspi (&grp_ctx->gl);
	      return eret;
	    }
	}

      if( pgaspi_dev_post_group_write((void *) barrier_ptr, 1, dst,
				      (void *) GPI2_GRP_REMOTE_SYNC_ADDR(grp_ctx, dst),
				      g) != 0)
	{
	  gctx->qp_state_vec[GASPI_COLL_QP][dst] = GASPI_STATE_CORRUPT;
	  unlock_gaspi (&grp_ctx->gl);
	  return GASPI_ERR_DEVICE;
	}

    B0:
      if( _gaspi_sync_wait(gctx, GPI2_GRP_SYNC_POLL_ADDR(grp_ctx, src), grp_ctx->barrier_cnt, timeout_ms )  != GASPI_SUCCESS )
	{
	  grp_ctx->lastmask = mask | 0x80000000;
	  unlock_gaspi (&grp_ctx->gl);
	  return GASPI_TIMEOUT;
	}

      mask <<= 1;
    }

  /* Note: at this point, it can happen that no or only some
     completions are polled. So far no problems have been observed but
     theoretically it is possible for the queue to become broken
     e.g. with a small, user-defined queue size and a large number of
     ranks. */

  const int pret = pgaspi_dev_poll_groups();
  if( pret < 0 )
    {
      unlock_gaspi (&grp_ctx->gl);
      return GASPI_ERR_DEVICE;
    }

  grp_ctx->togle = (grp_ctx->togle ^ 0x1);
  grp_ctx->coll_op = GASPI_NONE;
  grp_ctx->lastmask = 0x1;

  GPI2_STATS_INC_COUNT(GASPI_STATS_COUNTER_NUM_BARRIER, 1);
  GPI2_STATS_STOP_TIMER(GASPI_BARRIER_TIMER);
  GPI2_STATS_INC_TIMER(GASPI_STATS_TIME_BARRIER, GPI2_STATS_GET_TIMER(GASPI_BARRIER_TIMER));

  unlock_gaspi (&(grp_ctx->gl));

  return GASPI_SUCCESS;
}

/* Write allreduce data and sync flag */
static inline gaspi_return_t
_gaspi_allreduce_write_and_sync(gaspi_context_t * const gctx,
				const gaspi_group_t g,
				unsigned char* send_ptr,
				int buf_size,
				const gaspi_rank_t dst,
				int bid,
				const gaspi_timeout_t timeout_ms )
{
  gaspi_return_t eret = GASPI_ERROR;
  gaspi_group_ctx_t * const grp_ctx = &(glb_gaspi_group_ctx[g]);

  if( GASPI_ENDPOINT_DISCONNECTED == gctx->ep_conn[dst].cstat )
    {
      if( (eret = pgaspi_connect((gaspi_rank_t) dst, timeout_ms)) != GASPI_SUCCESS )
	{
	  gaspi_print_error("Failed to connect to rank %u", dst);
	  return eret;
	}
    }

  if( !grp_ctx->committed_rank[dst] )
    {
      if( (eret = _pgaspi_group_commit_to(g, dst, timeout_ms)) != GASPI_SUCCESS )
	{
	  gaspi_print_error("Failed to commit to rank %u", dst);
	  unlock_gaspi (&grp_ctx->gl);
	  return eret;
	}
    }

  void* remote_addr = (void *)(grp_ctx->rrcd[dst].data.addr + (COLL_MEM_RECV + (TOGGLE_SIZE * bid + grp_ctx->togle) * GPI2_REDUX_BUF_SIZE));
  if( pgaspi_dev_post_group_write(send_ptr, buf_size, dst, remote_addr, g) != 0 )
    {
      gctx->qp_state_vec[GASPI_COLL_QP][dst] = GASPI_STATE_CORRUPT;
      return GASPI_ERR_DEVICE;
    }

  unsigned char *barrier_ptr = GPI2_GRP_LOCAL_SYNC_ADDR(grp_ctx);
  barrier_ptr[0] = grp_ctx->barrier_cnt;

  if( pgaspi_dev_post_group_write(barrier_ptr, 1, dst, (void*) GPI2_GRP_REMOTE_SYNC_ADDR(grp_ctx, dst), g) != 0 )
    {
      gctx->qp_state_vec[GASPI_COLL_QP][dst] = GASPI_STATE_CORRUPT;
      return GASPI_ERR_DEVICE;
    }

  return GASPI_SUCCESS;
}

/* Apply default or user reduction */
static inline gaspi_return_t
_gaspi_apply_redux(gaspi_group_t g,
		   unsigned char** send_ptr,
		   unsigned char* recv_ptr,
		   int bid,
		   struct redux_args* r_args,
		   gaspi_timeout_t timeout_ms)

{
  gaspi_group_ctx_t * const grp_ctx = &(glb_gaspi_group_ctx[g]);
  gaspi_return_t eret = GASPI_ERROR;

  void * const dst_val = (void *) (recv_ptr + (2 * bid + grp_ctx->togle) * GPI2_REDUX_BUF_SIZE);
  void * const local_val = (void *) *send_ptr;

  /* Note: we're updating these arguments */
  const int dsize = (r_args->elem_size * r_args->elem_cnt);
  *send_ptr += dsize;
  grp_ctx->dsize += dsize;

  if( r_args->f_type == GASPI_OP )
    {
      gaspi_operation_t op = r_args->f_args.op;
      gaspi_datatype_t type = r_args->f_args.type;
      //TODO: magic number
      fctArrayGASPI[op * 6 + type] ((void *) *send_ptr, local_val, dst_val, r_args->elem_cnt);
      eret = GASPI_SUCCESS;
    }
  else if( r_args->f_type == GASPI_USER )
    {
      eret = r_args->f_args.user_fct( local_val, dst_val,
				      (void *) *send_ptr, r_args->f_args.rstate,
				      r_args->elem_cnt, r_args->elem_size, timeout_ms);
    }

  return eret;
}

static inline gaspi_return_t
_gaspi_allreduce (gaspi_context_t * const gctx,
		  const gaspi_pointer_t buf_send,
		  gaspi_pointer_t const buf_recv,
		  struct redux_args *r_args,
		  const gaspi_group_t g,
		  const gaspi_timeout_t timeout_ms)
{
  int idst, dst, bid = 0;
  int mask, tmprank, tmpdst;

  gaspi_group_ctx_t * const grp_ctx = &(glb_gaspi_group_ctx[g]);

  if( grp_ctx->level == 0 )
    {
      grp_ctx->barrier_cnt++;

      /* We need to take care of wraparound of the barrier_cnt */
      if( grp_ctx->barrier_cnt == 0 )
	{
	  grp_ctx->barrier_cnt++;
	}
    }

  const int rank_in_grp = grp_ctx->rank;

  unsigned char *send_ptr = grp_ctx->rrcd[gctx->rank].data.buf + COLL_MEM_SEND + (grp_ctx->togle * GASPI_COLL_OP_TYPES * GPI2_REDUX_BUF_SIZE);
  unsigned char *recv_ptr = grp_ctx->rrcd[gctx->rank].data.buf + COLL_MEM_RECV;

  const int dsize = r_args->elem_size * r_args->elem_cnt;
  memcpy (send_ptr, buf_send, dsize);

  const int rest = grp_ctx->tnc - grp_ctx->next_pof2;

  if( grp_ctx->level >= 2 )
    {
      bid = grp_ctx->bid;
      tmprank = grp_ctx->tmprank;
      send_ptr += grp_ctx->dsize;

      if( grp_ctx->level == 2 )
	{
	  goto L2;
	}
      else if( grp_ctx->level == 3 )
	{
	  goto L3;
	}
    }

  if( rank_in_grp < 2 * rest )
    {
      if( rank_in_grp % 2 == 0 )
	{
	  dst = grp_ctx->rank_grp[rank_in_grp + 1];

	  if( _gaspi_allreduce_write_and_sync(gctx, g, send_ptr, dsize, dst, bid, timeout_ms) != GASPI_SUCCESS )
	    {
	      gctx->qp_state_vec[GASPI_COLL_QP][dst] = GASPI_STATE_CORRUPT;
	      return GASPI_ERR_DEVICE;
	    }

	  tmprank = -1;
	}
      else
	{
	  if( _gaspi_sync_wait(gctx, GPI2_GRP_SYNC_POLL_ADDR(grp_ctx, (rank_in_grp - 1)), grp_ctx->barrier_cnt, timeout_ms)  != GASPI_SUCCESS )
	    {
	      grp_ctx->level = 1;
	      return GASPI_TIMEOUT;
	    }

	  if( _gaspi_apply_redux(g, &send_ptr, recv_ptr, bid, r_args, timeout_ms) != GASPI_SUCCESS )
	    {
	      return GASPI_ERROR;
	    }

	  tmprank = rank_in_grp >> 1;
	}

      bid++;
    }
  else
    {
      tmprank = rank_in_grp - rest;
      if( rest )
	{
	  bid++;
	}
    }

  grp_ctx->tmprank = tmprank;
  grp_ctx->bid = bid;
  grp_ctx->level = 2;

  //second phase
 L2:
  if( tmprank != -1 )
    {
      mask = grp_ctx->lastmask & 0x7fffffff;
      int jmp = grp_ctx->lastmask >> 31;

      while( mask < grp_ctx->next_pof2 )
	{
	  tmpdst = tmprank ^ mask;
	  idst = (tmpdst < rest) ? tmpdst * 2 + 1 : tmpdst + rest;
	  dst = grp_ctx->rank_grp[idst];

	  if( jmp )
	    {
	      jmp = 0;
	      goto J2;
	    }

	  if( _gaspi_allreduce_write_and_sync(gctx, g, send_ptr, dsize, dst, bid, timeout_ms) != GASPI_SUCCESS )
	    {
	      gctx->qp_state_vec[GASPI_COLL_QP][dst] = GASPI_STATE_CORRUPT;
	      return GASPI_ERR_DEVICE;
	    }
	J2:
	  if( _gaspi_sync_wait(gctx, GPI2_GRP_SYNC_POLL_ADDR(grp_ctx, idst), grp_ctx->barrier_cnt, timeout_ms)  != GASPI_SUCCESS )
	    {
	      grp_ctx->lastmask = (mask | 0x80000000);
	      grp_ctx->bid = bid;
	      return GASPI_TIMEOUT;
	    }

	  if( _gaspi_apply_redux(g, &send_ptr, recv_ptr, bid, r_args, timeout_ms) != GASPI_SUCCESS )
	    {
	      return GASPI_ERROR;
	    }

	  mask <<= 1;
	  bid++;
	}
    }

  grp_ctx->bid = bid;
  grp_ctx->level = 3;

  //third phase
 L3:
  if( rank_in_grp < 2 * rest )
    {
      if( rank_in_grp % 2 )
	{
	  dst = grp_ctx->rank_grp[rank_in_grp - 1];

	  if( _gaspi_allreduce_write_and_sync(gctx, g, send_ptr, dsize, dst, bid, timeout_ms) != GASPI_SUCCESS )
	    {
	      gctx->qp_state_vec[GASPI_COLL_QP][dst] = GASPI_STATE_CORRUPT;
	      return GASPI_ERR_DEVICE;
	    }
	}
      else
	{
	  if( _gaspi_sync_wait(gctx, GPI2_GRP_SYNC_POLL_ADDR(grp_ctx, rank_in_grp), grp_ctx->barrier_cnt, timeout_ms)  != GASPI_SUCCESS )
	    {
	      return GASPI_TIMEOUT;
	    }

	  bid += grp_ctx->pof2_exp;
	  send_ptr = (recv_ptr + (2 * bid + grp_ctx->togle) * GPI2_REDUX_BUF_SIZE);
	}
    }

  const int pret = pgaspi_dev_poll_groups();
  if( pret < 0 )
    {
      return GASPI_ERR_DEVICE;
    }

  grp_ctx->togle = (grp_ctx->togle ^ 0x1);

  grp_ctx->coll_op = GASPI_NONE;
  grp_ctx->lastmask = 0x1;
  grp_ctx->level = 0;
  grp_ctx->dsize = 0;
  grp_ctx->bid   = 0;

  memcpy(buf_recv, send_ptr, dsize);

  return GASPI_SUCCESS;
}

#pragma weak gaspi_allreduce = pgaspi_allreduce
gaspi_return_t
pgaspi_allreduce (const gaspi_pointer_t buf_send,
		  gaspi_pointer_t const buf_recv,
		  const gaspi_number_t elem_cnt,
		  const gaspi_operation_t op,
		  const gaspi_datatype_t type,
		  const gaspi_group_t g,
		  const gaspi_timeout_t timeout_ms)
{
  gaspi_verify_init("gaspi_allreduce_user");
  gaspi_verify_null_ptr(buf_send);
  gaspi_verify_null_ptr(buf_recv);
  gaspi_verify_group(g);

  if( elem_cnt > GPI2_ALLREDUCE_ELEM_MAX )
    {
      return GASPI_ERR_INV_NUM;
    }

  if( lock_gaspi_tout (&glb_gaspi_group_ctx[g].gl, timeout_ms ))
    {
      return GASPI_TIMEOUT;
    }

  if( !(glb_gaspi_group_ctx[g].coll_op & GASPI_ALLREDUCE) )
    {
      unlock_gaspi (&glb_gaspi_group_ctx[g].gl);
      return GASPI_ERR_ACTIVE_COLL;
    }

  glb_gaspi_group_ctx[g].coll_op = GASPI_ALLREDUCE;

  struct redux_args r_args;
  r_args.f_type = GASPI_OP;
  r_args.f_args.op = op;
  r_args.f_args.type = type;
  r_args.elem_size = glb_gaspi_typ_size[type];
  r_args.elem_cnt = elem_cnt;

  gaspi_return_t eret = GASPI_ERROR;
  gaspi_context_t * const gctx = &glb_gaspi_ctx;

  eret = _gaspi_allreduce(gctx, buf_send, buf_recv, &r_args, g, timeout_ms);


  unlock_gaspi (&glb_gaspi_group_ctx[g].gl);

  return eret;
}

#pragma weak gaspi_allreduce_user = pgaspi_allreduce_user
gaspi_return_t
pgaspi_allreduce_user (const gaspi_pointer_t buf_send,
		       gaspi_pointer_t const buf_recv,
		       const gaspi_number_t elem_cnt,
		       const gaspi_size_t elem_size,
		       gaspi_reduce_operation_t const user_fct,
		       gaspi_state_t const rstate,
		       const gaspi_group_t g,
		       const gaspi_timeout_t timeout_ms)
{
  gaspi_verify_init("gaspi_allreduce_user");
  gaspi_verify_null_ptr(buf_send);
  gaspi_verify_null_ptr(buf_recv);
  gaspi_verify_group(g);

  if( elem_cnt > GPI2_ALLREDUCE_ELEM_MAX )
    {
      return GASPI_ERR_INV_NUM;
    }

  if( elem_size * elem_cnt > GPI2_REDUX_BUF_SIZE )
    {
      return GASPI_ERR_INV_SIZE;
    }

  if( lock_gaspi_tout (&glb_gaspi_group_ctx[g].gl, timeout_ms) )
    {
      return GASPI_TIMEOUT;
    }

  if( !(glb_gaspi_group_ctx[g].coll_op & GASPI_ALLREDUCE_USER) )
    {
      unlock_gaspi (&glb_gaspi_group_ctx[g].gl);
      return GASPI_ERR_ACTIVE_COLL;
    }

  glb_gaspi_group_ctx[g].coll_op = GASPI_ALLREDUCE_USER;

  gaspi_return_t eret = GASPI_ERROR;
  gaspi_context_t * const gctx = &glb_gaspi_ctx;

  struct redux_args r_args;
  r_args.f_type = GASPI_USER;
  r_args.elem_size = elem_size;
  r_args.elem_cnt = elem_cnt;
  r_args.f_args.user_fct = user_fct;
  r_args.f_args.rstate = rstate;

  eret = _gaspi_allreduce(gctx, buf_send, buf_recv, &r_args, g, timeout_ms);

  unlock_gaspi (&glb_gaspi_group_ctx[g].gl);

  return eret;
}
