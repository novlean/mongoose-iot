/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#if MG_NET_IF == MG_NET_IF_LWIP_LOW_LEVEL

#ifndef MG_SIG_QUEUE_LEN
#define MG_SIG_QUEUE_LEN 16
#endif

struct mg_ev_mgr_lwip_signal {
  int sig;
  struct mg_connection *nc;
};

struct mg_ev_mgr_lwip_data {
  struct mg_ev_mgr_lwip_signal sig_queue[MG_SIG_QUEUE_LEN];
  int sig_queue_len;
  int start_index;
};

void mg_lwip_post_signal(enum mg_sig_type sig, struct mg_connection *nc) {
  struct mg_ev_mgr_lwip_data *md =
      (struct mg_ev_mgr_lwip_data *) nc->mgr->mgr_data;
  if (md->sig_queue_len >= MG_SIG_QUEUE_LEN) return;
  int end_index = (md->start_index + md->sig_queue_len) % MG_SIG_QUEUE_LEN;
  md->sig_queue[end_index].sig = sig;
  md->sig_queue[end_index].nc = nc;
  md->sig_queue_len++;
}

void mg_ev_mgr_lwip_process_signals(struct mg_mgr *mgr) {
  struct mg_ev_mgr_lwip_data *md = (struct mg_ev_mgr_lwip_data *) mgr->mgr_data;
  while (md->sig_queue_len > 0) {
    struct mg_connection *nc = md->sig_queue[md->start_index].nc;
    struct mg_lwip_conn_state *cs = (struct mg_lwip_conn_state *) nc->sock;
    switch (md->sig_queue[md->start_index].sig) {
      case MG_SIG_CONNECT_RESULT: {
        mg_if_connect_cb(nc, cs->err);
        break;
      }
      case MG_SIG_CLOSE_CONN: {
        nc->flags |= MG_F_CLOSE_IMMEDIATELY;
        mg_close_conn(nc);
        break;
      }
      case MG_SIG_SENT_CB: {
        if (cs->num_sent > 0) mg_if_sent_cb(nc, cs->num_sent);
        cs->num_sent = 0;
        break;
      }
      case MG_SIG_TOMBSTONE: {
        break;
      }
    }
    md->start_index = (md->start_index + 1) % MG_SIG_QUEUE_LEN;
    md->sig_queue_len--;
  }
}

void mg_ev_mgr_init(struct mg_mgr *mgr) {
  LOG(LL_INFO, ("%p Mongoose init"));
  mgr->mgr_data = MG_CALLOC(1, sizeof(struct mg_ev_mgr_lwip_data));
}

void mg_ev_mgr_free(struct mg_mgr *mgr) {
  MG_FREE(mgr->mgr_data);
  mgr->mgr_data = NULL;
}

void mg_ev_mgr_add_conn(struct mg_connection *nc) {
  (void) nc;
}

void mg_ev_mgr_remove_conn(struct mg_connection *nc) {
  struct mg_ev_mgr_lwip_data *md =
      (struct mg_ev_mgr_lwip_data *) nc->mgr->mgr_data;
  /* Walk the queue and null-out further signals for this conn. */
  for (int i = 0; i < MG_SIG_QUEUE_LEN; i++) {
    if (md->sig_queue[i].nc == nc) {
      md->sig_queue[i].sig = MG_SIG_TOMBSTONE;
    }
  }
}

time_t mg_mgr_poll(struct mg_mgr *mgr, int timeout_ms) {
  int n = 0;
  double now = mg_time();
  struct mg_connection *nc, *tmp;
  double min_timer = 0;
  int num_timers = 0;
  DBG(("begin poll @%u", (unsigned int) (now * 1000)));
  mg_ev_mgr_lwip_process_signals(mgr);
  for (nc = mgr->active_connections; nc != NULL; nc = tmp) {
    struct mg_lwip_conn_state *cs = (struct mg_lwip_conn_state *) nc->sock;
    (void) cs;
    tmp = nc->next;
    n++;
    if (nc->flags & MG_F_CLOSE_IMMEDIATELY) {
      mg_close_conn(nc);
      continue;
    }
    mg_if_poll(nc, now);
    mg_if_timer(nc, now);
    if (nc->send_mbuf.len == 0 && (nc->flags & MG_F_SEND_AND_CLOSE) &&
        !(nc->flags & MG_F_WANT_WRITE)) {
      mg_close_conn(nc);
      continue;
    }
#ifdef SSL_KRYPTON
    if (nc->ssl != NULL && cs != NULL && cs->pcb.tcp != NULL &&
        cs->pcb.tcp->state == ESTABLISHED) {
      if (((nc->flags & MG_F_WANT_WRITE) || nc->send_mbuf.len > 0) &&
          cs->pcb.tcp->snd_buf > 0) {
        /* Can write more. */
        if (nc->flags & MG_F_SSL_HANDSHAKE_DONE) {
          if (!(nc->flags & MG_F_CONNECTING)) mg_lwip_ssl_send(nc);
        } else {
          mg_lwip_ssl_do_hs(nc);
        }
      }
      if (cs->rx_chain != NULL || (nc->flags & MG_F_WANT_READ)) {
        if (nc->flags & MG_F_SSL_HANDSHAKE_DONE) {
          if (!(nc->flags & MG_F_CONNECTING)) mg_lwip_ssl_recv(nc);
        } else {
          mg_lwip_ssl_do_hs(nc);
        }
      }
    } else
#endif /* SSL_KRYPTON */
    {
      if (!(nc->flags & (MG_F_CONNECTING | MG_F_UDP))) {
        if (nc->send_mbuf.len > 0) mg_lwip_send_more(nc);
      }
    }
    if (nc->ev_timer_time > 0) {
      if (num_timers == 0 || nc->ev_timer_time < min_timer) {
        min_timer = nc->ev_timer_time;
      }
      num_timers++;
    }
  }
  DBG(("end poll @%u, %d conns, %d timers (min %u), next in %d ms",
       (unsigned int) (now * 1000), n, num_timers,
       (unsigned int) (min_timer * 1000), timeout_ms));
  return now;
}

uint32_t mg_lwip_get_poll_delay_ms(struct mg_mgr *mgr) {
  struct mg_connection *nc;
  double now = mg_time();
  double min_timer = 0;
  int num_timers = 0;
  mg_ev_mgr_lwip_process_signals(mgr);
  for (nc = mg_next(mgr, NULL); nc != NULL; nc = mg_next(mgr, nc)) {
    if (nc->ev_timer_time > 0) {
      if (num_timers == 0 || nc->ev_timer_time < min_timer) {
        min_timer = nc->ev_timer_time;
      }
      num_timers++;
    }
  }
  uint32_t timeout_ms = ~0;
  if (num_timers > 0) {
    double timer_timeout_ms = (min_timer - now) * 1000 + 1 /* rounding */;
    if (timer_timeout_ms < timeout_ms) {
      timeout_ms = timer_timeout_ms;
    }
  }
  return timeout_ms;
}

#endif /* MG_NET_IF == MG_NET_IF_LWIP_LOW_LEVEL */
