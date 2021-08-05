#pragma once

#include "types.h"
#include <netinet/in.h>
#include <event2/event.h>

struct Peer;

char * peer_find_handle(fingerprint_t fingerprint,
                        struct Peer * array, int num_peers);

// do_connect 1 => connect as well as track, 0 => track only
int peer_track(char *handle, fingerprint_t fingerprint, char *peer_address,
               struct Peer **array_inout, int *num_peers_inout,
               char *my_address, struct event_base *base, int do_connect);

void peers_free(struct Peer *array,
                int num_peers);

typedef void(*peer_ack_callback_t)(void *arg);

int peer_send_message(fingerprint_t my_fingerprint,
                      char * peer,
                      char * message,
                      struct Peer * peers,
                      int num_peers,
                      peer_ack_callback_t callback,
                      void *cbarg);

void peers_notify_new_handle(const char * handle,
                             fingerprint_t fingerprint,
                             struct Peer * peers,
                             int num_peers);

void peer_set_handle(const char * handle,
                     fingerprint_t fingerprint,
                     struct Peer * peers,
                     int num_peers);
