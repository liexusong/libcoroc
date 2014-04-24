#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "support.h"
#include "channel.h"
#include "vpu.h"
#include "coroutine.h"

TSC_SIGNAL_MASK_DECLARE

typedef struct quantum {
  bool close;
  bool select;
  tsc_chan_t chan;
  tsc_coroutine_t coroutine;
  uint8_t *itembuf;
  queue_item_t link;
} quantum;

static inline void quantum_init(quantum *q, tsc_chan_t chan,
                                tsc_coroutine_t coroutine, void *buf,
                                bool select) {
  q->close = false;
  q->select = select;
  q->chan = chan;
  q->coroutine = coroutine;
  q->itembuf = buf;
  queue_item_init(&q->link, q);
}

bool __tsc_copy_to_buff(tsc_chan_t chan, void *buf) {
  tsc_buffered_chan_t bchan = (tsc_buffered_chan_t)chan;

  if (bchan->nbuf < bchan->bufsize) {
    uint8_t *p = bchan->buf;
    p += (chan->elemsize) * (bchan->sendx++);
    __chan_memcpy(p, buf, chan->elemsize);
    (bchan->sendx) %= (bchan->bufsize);
    bchan->nbuf++;

    return true;
  }
  return false;
}

bool __tsc_copy_from_buff(tsc_chan_t chan, void *buf) {
  tsc_buffered_chan_t bchan = (tsc_buffered_chan_t)chan;

  if (bchan->nbuf > 0) {
    uint8_t *p = bchan->buf;
    p += (chan->elemsize) * (bchan->recvx++);
    __chan_memcpy(buf, p, chan->elemsize);
    (bchan->recvx) %= (bchan->bufsize);
    bchan->nbuf--;
    return true;
  }
  return false;
}

tsc_chan_t tsc_chan_allocate(int32_t elemsize, int32_t bufsize) {
  struct tsc_chan *chan;

  if (bufsize > 0) {
    tsc_buffered_chan_t bchan =
        TSC_ALLOC(sizeof(struct tsc_buffered_chan) + elemsize * bufsize);
    // init the buffered channel ..
    tsc_buffered_chan_init(bchan, elemsize, bufsize);
    chan = (tsc_chan_t)bchan;
  } else {
    chan = TSC_ALLOC(sizeof(struct tsc_chan));
    tsc_chan_init(chan, elemsize, NULL, NULL);
  }

  return chan;
}

void tsc_chan_dealloc(tsc_chan_t chan) {
  /* TODO: awaken the sleeping coroutines */
  lock_fini(&chan->lock);
  TSC_DEALLOC(chan);
}

static quantum *fetch_quantum(queue_t *queue) {
  quantum *q = NULL;
  while ((q = queue_rem(queue)) != NULL) {
    // if this quantum is generated by a select call,
    // only one task can fetch it!
    if (!(q->select) || TSC_CAS(&q->coroutine->qtag, NULL, q->chan)) break;
  }

  return q;
}

static int __tsc_chan_send(tsc_chan_t chan, void *buf, bool block) {
  tsc_coroutine_t self = tsc_coroutine_self();

  // check if there're any waiting coroutines ..
  quantum *qp = fetch_quantum(&chan->recv_que);
  if (qp != NULL) {
    __chan_memcpy(qp->itembuf, buf, chan->elemsize);
    vpu_ready(qp->coroutine);
    return CHAN_SUCCESS;
  }

  // check if the chan is closed by another coroutine
  if (chan->close) return CHAN_CLOSED;

  // check if there're any buffer slots ..
  if (chan->copy_to_buff && chan->copy_to_buff(chan, buf)) return CHAN_SUCCESS;

  // block or return CHAN_BUSY ..
  if (block) {
    // the async way ..
    quantum q;
    quantum_init(&q, chan, self, buf, false);
    queue_add(&chan->send_que, &q.link);
    vpu_suspend(&chan->lock, (unlock_handler_t)(lock_release));
    // awaken by a receiver later ..
    lock_acquire(&chan->lock);
    TSC_SYNC_ALL();
    if (q.close) return CHAN_CLOSED;
    return CHAN_AWAKEN;
  }

  return CHAN_BUSY;
}

static int __tsc_chan_recv(tsc_chan_t chan, void *buf, bool block) {
  tsc_coroutine_t self = tsc_coroutine_self();

  // check if there're any senders pending .
  quantum *qp = fetch_quantum(&chan->send_que);
  if (qp != NULL) {
    __chan_memcpy(buf, qp->itembuf, chan->elemsize);
    vpu_ready(qp->coroutine);
    return CHAN_SUCCESS;
  }
  // check if there're any empty slots ..
  if (chan->copy_from_buff && chan->copy_from_buff(chan, buf))
    return CHAN_SUCCESS;

  // check if the chan is closed by another coroutine
  if (chan->close) return CHAN_CLOSED;

  // block or return CHAN_BUSY
  if (block) {
    // async way ..
    quantum q;
    quantum_init(&q, chan, self, buf, false);
    queue_add(&chan->recv_que, &q.link);
    vpu_suspend(&chan->lock, (unlock_handler_t)(lock_release));
    // awaken by a sender later ..
    lock_acquire(&chan->lock);
    TSC_SYNC_ALL();

    if (q.close) return CHAN_CLOSED;
    return CHAN_AWAKEN;
  }

  return CHAN_BUSY;
}

/* -- the public APIs for tsc_chan -- */
int _tsc_chan_send(tsc_chan_t chan, void *buf, bool block) {
  TSC_SIGNAL_MASK();

  int ret;
  lock_acquire(&chan->lock);
  ret = __tsc_chan_send(chan, buf, block);
  lock_release(&chan->lock);

  TSC_SIGNAL_UNMASK();
  return ret;
}

int _tsc_chan_recv(tsc_chan_t chan, void *buf, bool block) {
  TSC_SIGNAL_MASK();
  int ret;

  // if the `chan' is nil, start the message passing mode..
  if (chan == NULL) chan = (tsc_chan_t)tsc_coroutine_self();
  lock_acquire(&chan->lock);
  ret = __tsc_chan_recv(chan, buf, block);
  lock_release(&chan->lock);

  TSC_SIGNAL_UNMASK();
  return ret;
}

int tsc_chan_close(tsc_chan_t chan) {
  int ret = CHAN_SUCCESS;

  TSC_SIGNAL_MASK();
  lock_acquire(&chan->lock);

  if (chan->close) {
    ret = CHAN_CLOSED;
  } else {
    // wakeup all coroutines waiting for this chan
    volatile quantum *qp = NULL;
    chan->close = true;
    while ((qp = fetch_quantum(&chan->send_que)) != NULL) {
      qp->close = true;
      TSC_SYNC_ALL();
      vpu_ready(qp->coroutine);
    }

    while ((qp = fetch_quantum(&chan->recv_que)) != NULL) {
      qp->close = true;
      TSC_SYNC_ALL();
      vpu_ready(qp->coroutine);
    }
  }

  lock_release(&chan->lock);
  TSC_SIGNAL_UNMASK();

  return ret;
}

#if defined(ENABLE_CHANNEL_SELECT)

/* -- the public APIs for channel select -- */
tsc_chan_set_t tsc_chan_set_allocate(int n) {
  tsc_chan_set_t set = TSC_ALLOC(sizeof(struct tsc_chan_set) +
                                 n * sizeof(tsc_scase_t) + n * sizeof(lock_t));
  assert(set != NULL);

  set->sorted = false;
  set->volume = n;
  set->size = 0;
  set->locks = (lock_t *)(&set->cases[n]);

  return set;
}

void tsc_chan_set_dealloc(tsc_chan_set_t set) { TSC_DEALLOC(set); }

void tsc_chan_set_send(tsc_chan_set_t set, tsc_chan_t chan, void *buf) {
  assert(set != NULL && chan != NULL);
  assert(set->size < set->volume);

  int i = set->size++;
  tsc_scase_t *scase = &(set->cases[i]);

  scase->type = CHAN_SEND;
  scase->chan = chan;
  scase->buf = buf;

  set->locks[i] = &chan->lock;
  chan->select = true;
}

void tsc_chan_set_recv(tsc_chan_set_t set, tsc_chan_t chan, void *buf) {
  assert(set != NULL);
  assert(set->size < set->volume);
  // if the `chan' is nil, start the message-passing mode ..
  if (chan == NULL) chan = (tsc_chan_t)tsc_coroutine_self();

  int i = set->size++;
  tsc_scase_t *scase = &(set->cases[i]);

  scase->type = CHAN_RECV;
  scase->chan = chan;
  scase->buf = buf;

  set->locks[i] = &chan->lock;
  chan->select = true;
}

int _tsc_chan_set_select(tsc_chan_set_t set, bool block, tsc_chan_t *active) {
  assert(set != NULL);
  TSC_SIGNAL_MASK();

  if (set->size == 0) {
    *active = NULL;
    return -1;
  }

  int ret;
  tsc_coroutine_t self = tsc_coroutine_self();

  /* before lock the lock_chain, ensure that the locks in the chain
   * are sorted by their address, to avoid the deadlock !! */
  lock_chain_acquire((lock_chain_t *)set);

  int i;
  for (i = 0; i < set->size; i++) {
    tsc_scase_t *e = &set->cases[i];

    switch (e->type) {
      case CHAN_SEND:
        ret = __tsc_chan_send(e->chan, e->buf, false);
        break;
      case CHAN_RECV:
        ret = __tsc_chan_recv(e->chan, e->buf, false);
    }
    if (ret == CHAN_SUCCESS) {
      *active = e->chan;
      goto __leave_select;
    }
  }

  // got here means we can not get any active chan event
  // so block or return CHAN_BUSY ..
  if (block) {
    // TODO : add quantums ..
    self->qtag = NULL;
    quantum *qarray = TSC_ALLOC((set->size) * sizeof(quantum));
    quantum *pq = qarray;

    for (i = 0; i < set->size; i++) {
      tsc_scase_t *e = &set->cases[i];
      quantum_init(pq, e->chan, self, e->buf, true);
      switch (e->type) {
        case CHAN_SEND:
          queue_add(&e->chan->send_que, &pq->link);
          break;
        case CHAN_RECV:
          queue_add(&e->chan->recv_que, &pq->link);
          break;
      }
      pq++;
    }

    vpu_suspend(set, (unlock_handler_t)lock_chain_release);
    lock_chain_acquire((lock_chain_t *)set);

    // get the selected one
    *active = (tsc_chan_t)(self->qtag);
    ret = CHAN_SUCCESS;

    // dequeue all unactive chans ..
    pq = qarray;

    for (i = 0; i < set->size; i++) {
      tsc_scase_t *e = &set->cases[i];
      queue_t *que = &(e->chan->send_que);
      if (e->type == CHAN_RECV) que = &(e->chan->recv_que);

      // check if the selected one is closed ..
      if ((*active == pq->chan) && pq->close) ret = CHAN_CLOSED;

      queue_extract(que, &pq->link);
      pq++;
    }

    // relase the quantums , safe ?
    TSC_DEALLOC(qarray);

    *active = (tsc_chan_t)(self->qtag);
    self->qtag = NULL;
  }

__leave_select:
  lock_chain_release((lock_chain_t *)set);
  TSC_SIGNAL_UNMASK();
  return ret;
}

#endif  // ENABLE_CHANNEL_SELECT
