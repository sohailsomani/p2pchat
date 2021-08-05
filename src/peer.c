#include "peer.h"
#include "log.h"
#include "rpc.h"
#include <arpa/inet.h>
#include <assert.h>
#include <event2/http.h>
#include <event2/http_compat.h>
#include <event2/rpc.h>
#include <event2/rpc_struct.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct Peer {
  fingerprint_t fingerprint;
  char *handle;

  struct sockaddr_in sin;
  struct event_base *base;

  struct evrpc_pool *pool;
  struct evhttp_connection *connection;
};

static int peer_parse_address(char *address, struct sockaddr_in *sin) {
  if (!sin)
    return -1;

  memset(sin, 0, sizeof(struct sockaddr_in));
  char *s = strstr(address, ":");
  if (!s)
    return -1;

  *s = 0; // null terminate ip address temporarily
  int ret = inet_pton(AF_INET, address, &sin->sin_addr);
  *s = ':';

  if (ret <= 0) {
    if (ret < 0)
      perror("Unable to parse address");
    LOG_ERROR("Unable to parse address: %s", address);
    return -1;
  }

  const int base = 10;
  long int port = strtol(s + 1, 0, base);
  if (port == LONG_MIN || port == LONG_MAX) {
    LOG_ERROR0("Overflow in port???");
    return -1;
  }

  sin->sin_port = htons(port);
  ;

  return 0;
}

static struct Peer *find_or_add_peer(char *address, struct Peer **array,
                                     int *num_peers) {
  if (!num_peers)
    return 0;

  struct sockaddr_in sin;
  if (peer_parse_address(address, &sin) == -1)
    goto failure1;

  struct Peer *peer = 0;
  for (int ii = 0; ii < *num_peers; ++ii) {
    struct Peer *p = &(*array)[ii];
    if (memcmp(&sin, &p->sin, sizeof(struct sockaddr_in)) == 0) {
      peer = p;
      break;
    }
  }

  if (peer) {
    LOG_INFO("Found existing peer: %s#%d", peer->handle, peer->fingerprint);
  } else {
    LOG_INFO0("Creating new connection");
    struct Peer *array_alloc =
        reallocarray(*array, *num_peers + 1, sizeof(struct Peer));
    if (!array_alloc) {
      LOG_ERROR0("Could not allocate space for new peer!");
      goto failure2;
    }
    *array = array_alloc;
    *num_peers += 1;
    peer = &(*array)[*num_peers - 1];
    memset(peer, 0, sizeof(struct Peer));
  }

  (void)memcpy(&peer->sin, &sin, sizeof(sin));

  LOG_DEBUG("Working with peer: %s#%d", peer->handle, peer->fingerprint);
  return peer;
failure2:
failure1:
  return 0;
}

static void peer_free_rpc(struct Peer *peer) {
  if (peer->connection) {
    if (peer->pool)
      evrpc_pool_remove_connection(peer->pool, peer->connection);
    evhttp_connection_free_on_completion(peer->connection);
  }

  if (peer->pool)
    evrpc_pool_free(peer->pool);

  peer->pool = 0;
  peer->connection = 0;
}

static int peer_setup_rpc(struct Peer *peer) {
  int ret = -1;

  peer_free_rpc(peer);

  peer->pool = evrpc_pool_new(peer->base);
  if (!peer->pool)
    goto failure1;

  // Pool will set the base when we add the connection
  char * address = inet_ntoa(peer->sin.sin_addr); // NOLINT(concurrency-mt-unsafe)
  peer->connection = evhttp_connection_base_new(
      0, 0, address, ntohs(peer->sin.sin_port));
  if (!peer->connection)
    goto failure2;

  evrpc_pool_add_connection(peer->pool, peer->connection);

  ret = 0;
  goto exit;

failure2:
  evrpc_pool_free(peer->pool);
  peer->pool = 0;
failure1:
exit:
  return ret;
}

static void peer_free(struct Peer *peer) {
  LOG_INFO("Freeing peer: %s:%d", inet_ntoa(peer->sin.sin_addr), // NOLINT
           ntohs(peer->sin.sin_port));
  peer_free_rpc(peer);
  free(peer->handle);
}

static void connect_cb(struct evrpc_status *status,
                       struct ConnectRequest *request,
                       struct ConnectReply *reply, void *cbarg) {
  LOG_INFO0("New connection");

  struct Peer *peer = CAST(struct Peer *, cbarg);
  if (status->error != EVRPC_STATUS_ERR_NONE) {
    LOG_ERROR("Failed to connect: %d", status->error);
    goto failure1;
  }

  char *handle = 0;
  if (EVTAG_GET(reply, handle, &handle) == -1 || handle == 0)
    goto failure3;

  uint32_t fingerprint = 0;
  if (EVTAG_GET(reply, fingerprint, &fingerprint) == -1)
    goto failure2;
  peer->fingerprint = fingerprint;

  free(peer->handle);
  size_t size = strlen(handle) + 1;
  peer->handle = malloc(size);
  (void)strncpy(peer->handle, handle, size);
  assert(peer->handle[size - 1] == 0);

  LOG_INFO("Connected to peer %s#%d", peer->handle, peer->fingerprint);

  goto exit;

failure3:
failure2:
failure1:
exit:
  ConnectReply_free(reply);
  ConnectRequest_free(request);
}

int peer_track(char *handle, fingerprint_t fingerprint, char *peer_address,
               struct Peer **array, int *num_peers, char *my_address,
               struct event_base *base, int do_connect) {
  assert(base != 0);
  int ret = -1;
  struct Peer *peer = find_or_add_peer(peer_address, array, num_peers);
  if (!peer)
    goto failure1;

  if(peer->handle)
    free(peer->handle);
  size_t size = strlen(handle) + 1;
  peer->handle = malloc(size);
  (void)strncpy(peer->handle,handle,size);
  assert(peer->handle[size-1] == 0);

  peer->fingerprint = fingerprint;

  peer->base = base;
  if (peer_setup_rpc(peer) == -1)
    goto failure2;

  struct ConnectRequest *request = 0;
  struct ConnectReply *reply = 0;
  if (do_connect) {
    request = ConnectRequest_new();
    reply = ConnectReply_new();

    (void)EVTAG_ASSIGN(request, handle, handle);
    (void)EVTAG_ASSIGN(request, fingerprint, fingerprint);
    (void)EVTAG_ASSIGN(request, address, my_address);

    if (EVRPC_MAKE_REQUEST(Connect, peer->pool, request, reply, connect_cb,
                           peer) == -1)
      goto failure3;
  }

  ret = 0;
  goto exit;

failure3:
  ConnectReply_free(reply);
  ConnectRequest_free(request);
failure2:
failure1:
exit:
  return ret;
}

void peers_free(struct Peer *array, int num_peers) {
  for (int ii = 0; ii < num_peers; ++ii) {
    peer_free(&array[ii]);
  }
  free(array);
}

static int peer_parse_handle(char *peer, char **handle_out,
                             fingerprint_t *fingerprint_out) {
  int ret = -1;

  char *save = 0;
  char *handle = strtok_r(peer, "#", &save);
  if (!handle) {
    LOG_ERROR("Expected a handle, got %s", peer);
    goto failure1;
  }

  char *sfingerprint = peer + strlen(handle) + 1;
  const int base = 10;
  long fingerprint = strtol(sfingerprint, NULL, base);
  if (fingerprint == LONG_MIN || fingerprint == LONG_MAX) {
    LOG_ERROR0("Overflow in fingerprint");
    goto failure2;
  }

  *handle_out = handle;
  *fingerprint_out = fingerprint;

  ret = 0;
  goto exit;

failure2:
failure1:
exit:
  return ret;
}

static struct Peer *find_peer_by_fingerprint_handle(const char *handle, // optional
                                                    fingerprint_t fingerprint,
                                                    struct Peer *peers,
                                                    int num_peers) {
  assert(fingerprint != 0);
  for (int ii = 0; ii < num_peers; ++ii) {
    struct Peer *peer = &peers[ii];
    LOG_DEBUG("Checking %s#%d against %s#%d",
              handle ? handle : "(null)",
              fingerprint, peer->handle,
              peer->fingerprint);
    if (peer->fingerprint == fingerprint &&
        (!handle || strncmp(handle, peer->handle, strlen(handle)) == 0)) {
      return &peers[ii];
    }
  }
  return 0;
}

struct MessageCBData {
  peer_ack_callback_t callback;
  void *cbarg;
};

static void message_cb(struct evrpc_status *status,
                       struct MessageRequest * request,
                       struct MessageReply *reply,
                       void *cbarg) {
  struct MessageCBData *msg = CAST(struct MessageCBData*, cbarg);

  if(status->error != EVRPC_STATUS_ERR_NONE) {
    LOG_ERROR("Failed to send message: %d", status->error);
    goto failure1;
  }

  LOG_DEBUG0("Calling message ack callback");
  msg->callback(msg->cbarg);

  goto cleanup;
 cleanup:
 failure1:
  free(msg);
  MessageRequest_free(request);
  MessageReply_free(reply);
}

int peer_send_message(fingerprint_t my_fingerprint, char *speer, char *message,
                      struct Peer *peers, int num_peers,
                      peer_ack_callback_t callback, void *cbarg) {
  int ret = -1;

  char *handle = 0;
  fingerprint_t fingerprint = 0;

  if (peer_parse_handle(speer, &handle, &fingerprint) == -1) {
    LOG_ERROR("Unable to parse peer: %s", speer);
    goto failure1;
  }

  LOG_DEBUG("Parsed peer %s#%d", handle, fingerprint);

  struct Peer *peer =
      find_peer_by_fingerprint_handle(handle, fingerprint, peers, num_peers);
  if (!peer) {
    LOG_ERROR0(
        "Unable to find peer to send message, maybe they've never connected?");
    goto failure2;
  }

  LOG_DEBUG("Found peer %s#%d", peer->handle, peer->fingerprint);

  struct MessageRequest *request = MessageRequest_new();
  struct MessageReply *reply = MessageReply_new();

  (void)EVTAG_ASSIGN(request, message, message);
  (void)EVTAG_ASSIGN(request, fingerprint, my_fingerprint);

  struct MessageCBData *msg = malloc(sizeof(struct MessageCBData));
  msg->callback = callback;
  msg->cbarg = cbarg;

  if (EVRPC_MAKE_REQUEST(Message, peer->pool, request, reply, message_cb,
                         msg) == -1) {
    LOG_ERROR0("Unable to send message");
    goto failure3;
  }

  (void)message;
  (void)callback;
  (void)cbarg;

  ret = 0;
  goto exit;

failure3:
  free(msg);
  MessageRequest_free(request);
  MessageReply_free(reply);
failure1:
failure2:
exit:
  return ret;
}

char *peer_find_handle(fingerprint_t fingerprint, struct Peer *array,
                       int num_peers) {
  for(int ii = 0; ii < num_peers; ++ii) {
    struct Peer * peer = &array[ii];
    if(peer->fingerprint == fingerprint)
      return peer->handle;
  }
  return 0;
}

static void handle_change_cb(struct evrpc_status *status,
                             struct HandleChangeRequest *request,
                             struct HandleChangeReply *reply,
                             void *cbarg) {
  (void)status; (void)cbarg;
  HandleChangeRequest_free(request);
  HandleChangeReply_free(reply);
}

void peers_notify_new_handle(const char *handle, fingerprint_t fingerprint,
                             struct Peer *peers, int num_peers) {
  for(int ii = 0; ii < num_peers; ++ii) {
    struct Peer * peer = &peers[ii];

    struct HandleChangeRequest *request = HandleChangeRequest_new();
    struct HandleChangeReply *reply = HandleChangeReply_new();

    (void)EVTAG_ASSIGN(request,handle,handle);
    (void)EVTAG_ASSIGN(request,fingerprint,fingerprint);

    if(EVRPC_MAKE_REQUEST(HandleChange,peer->pool,request,reply,handle_change_cb,
                          NULL) == -1) {
      LOG_ERROR("Unable to notify %s", peer->handle);
      HandleChangeRequest_free(request);
      HandleChangeReply_free(reply);
    }
  }
}

void peer_set_handle(const char *handle, fingerprint_t fingerprint,
                     struct Peer *peers, int num_peers) {
  struct Peer *peer = find_peer_by_fingerprint_handle(NULL,fingerprint,peers,num_peers);
  if(!peer) {
    LOG_ERROR("Could not find peer with this fingerprint! %d", fingerprint);
    return;
  }

  free(peer->handle);
  size_t len = strlen(handle)+1;
  peer->handle = malloc(len);
  (void)strncpy(peer->handle,handle,len);
  assert(peer->handle[len-1] == 0);

  LOG_INFO("Set peer with fingerprint %d handle to %s", fingerprint, peer->handle);
}
