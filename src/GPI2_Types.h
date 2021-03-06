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

#ifndef _GPI2_TYPES_H_
#define _GPI2_TYPES_H_ 1

#include <pthread.h>

#include "GASPI.h"
#include "GPI2_CM.h"
#include "GASPI_Ext.h"

#define ALIGN64  __attribute__ ((aligned (64)))

/* Macro-ed constants */
#define GASPI_MAJOR_VERSION (1)
#define GASPI_MINOR_VERSION (3)
#define GASPI_REVISION (0)

#define GASPI_MAX_GROUPS  (32)
#define GASPI_MAX_MSEGS   (32)
#define GASPI_MAX_QP      (16)
#define GASPI_COLL_QP     (GASPI_MAX_QP)
#define GASPI_PASSIVE_QP  (GASPI_MAX_QP+1)
#define GASPI_SN          (GASPI_MAX_QP+2)
#define GASPI_MAX_TSIZE_C ((1ul<<31ul)-1ul)
#define GASPI_MAX_TSIZE_P ((1ul<<16ul)-1ul)
#define GASPI_MAX_QSIZE   (4096)
#define GASPI_MAX_NOTIFICATION  (65536)
#define GASPI_MAX_NUMAS   (4)

typedef struct
{
  ALIGN64 volatile unsigned char lock;
  char dummy[63];
} gaspi_lock_t;

typedef struct
{
  union
  {
    unsigned char *buf;
    void *ptr;
    unsigned long addr;
  } data;

  union
  {
    unsigned char *buf;
    void *ptr;
    unsigned long addr;
  } notif_spc;

  void* mr[2];

#ifdef GPI2_DEVICE_IB
  unsigned int rkey[2];
#endif

  unsigned long size;
  size_t notif_spc_size;
  int trans; /* info transmitted */

  int user_provided;
  gaspi_memory_description_t desc;

#ifdef GPI2_CUDA
  int cuda_dev_id; //cuda device id holding the memory
  union
  {
    void *host_ptr;
    unsigned long host_addr;
  };
  void *host_mr;
  unsigned int host_rkey;
#endif
} gaspi_rc_mseg_t;

typedef struct
{
  int localSocket; //TODO: rename?
  int rank;
  int tnc;
  float mhz;
  float cycles_to_msecs;
  char mfile[1024];
  int *sockfd;
  char *hn_poff;
  char *poff;
  int group_cnt;

  int mseg_cnt;
  unsigned char *qp_state_vec[GASPI_MAX_QP + 3];
  char mtyp[64];
  gaspi_lock_t lockPS;
  gaspi_lock_t lockPR;
  gaspi_lock_t lockC[GASPI_MAX_QP];
  pthread_t snt;

#ifdef GPI2_CUDA
  gaspi_number_t gpu_count;
  int use_gpus;
#endif

  gaspi_rc_mseg_t nsrc;
  gaspi_rc_mseg_t* rrmd[GASPI_MAX_MSEGS];

  gaspi_endpoint_conn_t *ep_conn;

  /* Number of "created" communication queues */
  gaspi_number_t num_queues;

  /* Comm counters  */
  int ne_count_grp;
  int ne_count_c[GASPI_MAX_QP];
  unsigned char ne_count_p[8192]; //TODO: dynamic size

} gaspi_context_t;

#endif /* _GPI2_TYPES_H_ */
