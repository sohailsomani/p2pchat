#include "app.h"
#include "event2/bufferevent.h"
#include "generated/rpc.h"
#include "log.h"
#include "peer.h"
#include "rpc.h"
#include "types.h"
#include <arpa/inet.h>
#include <assert.h>
#include <event2/event.h>
#include <event2/event_compat.h>
#include <event2/http.h>
#include <event2/rpc.h>
#include <event2/util.h>
#include <netinet/in.h>
#include <readline/readline.h>
#include <readline/tilde.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct Application { // NOLINT(altera-struct-pack-align)
  char *address; // address to send to peers to let them connect to us
  char *handle;
  fingerprint_t fingerprint;
  struct event_base *base;
  struct evhttp *http;
  struct evrpc_base *rpc;

  struct Peer *peers;
  int num_peers;
};

/***********
app_run
************/
static evutil_socket_t app_init_socket(struct Application *app);
static void log_unhandled_requests(struct evhttp_request *req, void *ignored);

static int app_setup_prompt(struct Application *app, struct event *event_stdin);
static void app_cleanup_prompt(struct Application *app,
                               struct event *event_stdin);

int app_run(struct Application *app) {
  int ret = EXIT_FAILURE;

  evutil_socket_t server_socket = app_init_socket(app);
  if (server_socket == -1)
    goto failure1;

  if (evhttp_accept_socket(app->http, server_socket) == -1)
    goto failure2;

  evhttp_set_gencb(app->http, log_unhandled_requests, NULL);

  struct event *event_stdin = malloc(event_get_struct_event_size());
  if (app_setup_prompt(app, event_stdin) == -1)
    goto failure3;

  LOG_DEBUG0("Starting event loop");
  (void)event_base_dispatch(app->base);
  LOG_DEBUG("Event loop done, exit code: %d", ret);

  ret = EXIT_SUCCESS;
  goto cleanup;

cleanup:
  app_cleanup_prompt(app, event_stdin);
failure3:
  free(event_stdin);
failure2:
  evutil_closesocket(server_socket);
failure1:
  (void)exit;
  /* exit: */
  return ret;
}

/********************
 RPC
********************/

static void connect_cb(EVRPC_STRUCT(Connect) * rpc, void *arg);
static void message_cb(EVRPC_STRUCT(Message) * rpc, void *arg);
static void handle_cb(EVRPC_STRUCT(HandleChange) *rpc, void *arg);

/********
 Peer stuff
*********/
static void app_connect_peer(struct Application *app, char *peer_address) {
  if (peer_track(app->handle, app->fingerprint, peer_address, &app->peers,
                 &app->num_peers, app->address, app->base,
                 /*do_connect*/ 1) == -1) {
    LOG_ERROR0("Could not start connection");
  }
}

static void app_ack_message_cb(void *cbarg) {
  struct Application *app = CAST(struct Application *, cbarg);
  (void)app;
}

/****************
app_new/app_free
****************/
struct Application *app_new(ApplicationConfig *cfg) {
  if (getenv("LIBEVENT_DEBUG")) // NOLINT(concurrency-mt-unsafe)
    event_enable_debug_logging(EVENT_DBG_ALL);

  (void)event_init(); // without this, everything breaks

  LOG_INFO("PID: %d", getpid());

  LOG_DEBUG0("Initializing app");
  struct Application *app =
      (struct Application *)malloc(sizeof(struct Application));

  if (!app)
    goto failure1;

  app->address = 0;
  app->fingerprint = cfg->fingerprint;

  const int HANDLE_LEN = 64;
  app->handle = malloc(sizeof(char) * (HANDLE_LEN + 1));
  if (!app->handle)
    goto failure3;
  app->handle[HANDLE_LEN] = 0;

  int ret = getlogin_r(app->handle, HANDLE_LEN);
  if (ret != 0)
    goto failure4;

  app->base = event_base_new();

  if (!app->base)
    goto failure5;
  LOG_DEBUG0("Initialized event loop");

  app->http = evhttp_new(app->base);
  if (!app->http)
    goto failure6;
  LOG_DEBUG0("Initialized HTTP server");

  app->rpc = evrpc_init(app->http);
  if (!app->rpc)
    goto failure7;

  if (EVRPC_REGISTER(app->rpc, Connect, ConnectRequest, ConnectReply,
                     connect_cb, app) == -1)
    goto failure8;

  if (EVRPC_REGISTER(app->rpc, Message, MessageRequest, MessageReply,
                     message_cb, app) == -1)
    goto failure9;

  if (EVRPC_REGISTER(app->rpc, HandleChange, HandleChangeRequest,
                     HandleChangeReply, handle_cb, app) == -1)
    goto failure10;

  LOG_DEBUG0("Initialized RPC server");

  app->peers = 0;
  app->num_peers = 0;

  LOG_DEBUG0("Done initializing app");
  return app;

failure10:
failure9:
failure8:
  evrpc_free(app->rpc);
failure7:
  evhttp_free(app->http);
failure6:
  event_base_free(app->base);
failure5:
failure4:
  free(app->handle);
failure3:
  free(app);
failure1:
  return 0;
}

void app_free(struct Application *app) {
  free(app->handle);
  free(app->address);
  (void)EVRPC_UNREGISTER(app->rpc, Connect);
  (void)EVRPC_UNREGISTER(app->rpc, Message);
  (void)EVRPC_UNREGISTER(app->rpc, HandleChange);
  evrpc_free(app->rpc);
  evhttp_free(app->http);
  event_base_free(app->base);
  peers_free(app->peers, app->num_peers);
  free(app);
  libevent_global_shutdown();
}

/*********************
  RPC IMPLEMENTATION
 ********************/
static void connect_cb(EVRPC_STRUCT(Connect) * rpc, void *arg) {
  struct Application *app = CAST(struct Application *, arg);

  LOG_DEBUG0("Got connection");
  struct ConnectRequest *request = rpc->request;
  struct ConnectReply *reply = rpc->reply;

  uint32_t fingerprint_in = 0;
  if (EVTAG_GET(request, fingerprint, &fingerprint_in) == -1)
    goto failure;

  fingerprint_t fingerprint = fingerprint_in;

  char *handle = 0;
  if (EVTAG_GET(request, handle, &handle) == -1)
    goto failure;

  char *peer_address = 0;
  if (EVTAG_GET(request, address, &peer_address) == -1)
    goto failure;

  if (peer_track(handle, fingerprint, peer_address, &app->peers,
                 &app->num_peers, app->address, app->base,
                 /*do_connect*/ 0) == -1) {
    LOG_ERROR0("Could not add peer connection");
    goto failure;
  }

  LOG_INFO("New connection, remote peer %s#%d from %s", handle, fingerprint,
           peer_address);

  (void)EVTAG_ASSIGN(reply, fingerprint, app->fingerprint);
  (void)EVTAG_ASSIGN(reply, handle, app->handle);

failure:
  EVRPC_REQUEST_DONE(rpc);
}

static void message_cb(EVRPC_STRUCT(Message) * rpc, void *arg) {
  struct Application *app = CAST(struct Application *, arg);

  struct MessageRequest *request = rpc->request;

  char *message = 0;
  uint32_t fingerprint = 0;

  if (EVTAG_GET(request, message, &message) == -1)
    goto failure1;

  if (EVTAG_GET(request, fingerprint, &fingerprint) == -1)
    goto failure2;

  char *handle = peer_find_handle(fingerprint, app->peers, app->num_peers);
  LOG_INFO("%s#%d says: %s", handle, fingerprint, message);

failure2:
failure1:
  EVRPC_REQUEST_DONE(rpc);
}

static void handle_cb(EVRPC_STRUCT(HandleChange) *rpc, void *arg) {
  struct Application *app = CAST(struct Application *, arg);

  struct HandleChangeRequest *request = rpc->request;

  char *new_handle = 0;
  uint32_t fingerprint = 0;

  if (EVTAG_GET(request, handle, &new_handle) == -1)
    goto failure1;

  if (EVTAG_GET(request, fingerprint, &fingerprint) == -1)
    goto failure2;

  char * curr_handle = peer_find_handle(fingerprint, app->peers,app->num_peers);
  LOG_INFO("Peer with fingerprint %d changing handle from %s to %s", fingerprint,curr_handle,new_handle);
  peer_set_handle(new_handle,fingerprint,app->peers,app->num_peers);

failure2:
failure1:
  EVRPC_REQUEST_DONE(rpc);
}

static void log_unhandled_requests(struct evhttp_request *req, void *ignored) {
  (void)ignored;
  LOG_DEBUG("Got unhandled request: %d", evhttp_request_get_command(req));
  evhttp_send_error(req, HTTP_BADREQUEST, "Unknown request");
}

static evutil_socket_t app_init_socket(struct Application *app) {
  evutil_socket_t server_socket = socket(AF_INET, SOCK_STREAM, 0);

  struct sockaddr_in sin = {0};
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = 0;
  sin.sin_port = htons(0);

  int reuseaddr_opt_val = 1;
  if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_opt_val,
                 sizeof(int))) {
    perror("Could not retrieve socket information");
    goto failure;
  }

  if (server_socket == -1) {
    perror("socket() failed");
    goto failure;
  }

  if (evutil_make_socket_nonblocking(server_socket) == -1) {
    perror("Could not make socket nonblocking");
    goto failure;
  }

  const int MAX_BACKLOG_LENGTH=16;
  if (bind(server_socket, (void *)&sin, sizeof(sin)) == -1 ||
      listen(server_socket, MAX_BACKLOG_LENGTH) == -1) {
    LOG_ERROR0("Could not bind socket");
    perror("Could not bind socket");
    goto failure;
  }

  memset(&sin, 0, sizeof(sin));
  socklen_t size = sizeof(sin);
  if (getsockname(server_socket, (void *)&sin, &size) == -1) {
    LOG_ERROR0("Could not get bound socket");
    perror("Could not get bound socket");
    goto failure;
  }

  char *addr = inet_ntoa(sin.sin_addr); // NOLINT(concurrency-mt-unsafe)
  const int MAX_NUM_CHARS_IN_PORT=6;
  size_t size2 = strlen(addr) + MAX_NUM_CHARS_IN_PORT + 1;
  char *bound_addr = malloc(size2);
  (void)snprintf(bound_addr, size, "%s:%d", addr, ntohs(sin.sin_port));
  if (app->address)
    free(app->address);

  app->address = bound_addr;

  LOG_DEBUG("Listening on %s", bound_addr);
  LOG_INFO("Ask your friends to use %s to connect to you", bound_addr);

  return server_socket;

failure:
  if (server_socket != -1)
    (void)evutil_closesocket(server_socket);
  return -1;
}

/***************
  Interactive prompt/commands
 ***************/
static struct Application *g_app_readline = 0; // NOLINT for readline callback only

static void app_set_handle(struct Application *app, char *handle);

typedef struct { // NOLINT(altera-struct-pack-align)
  const char *command;
  const char *help;
  void (*callback)(struct Application *, char *rest_of_line);
} command;

static void command_show_handle(struct Application *app, char *rest_of_line) { // NOLINT(readability-non-const-parameter)
  (void)rest_of_line;
  LOG_INFO("%s", app->handle);
}

static void command_set_handle(struct Application *app, char *rest_of_line) {
  app_set_handle(app, rest_of_line);
}

static void command_connect_peer(struct Application *app, char *address) {
  app_connect_peer(app, address);
}

static void command_show_help(struct Application *app, char * /*ignored*/);

const command g_commands[] = {
    {"/handle ", "/handle <handle>: set handle", command_set_handle},
    {"/handle", "/handle: show current handle", command_show_handle},
    {"/connect ", "/connect ipaddr:port: Connect to peer",
     command_connect_peer},
    {"/help", "/help: show help", command_show_help}};

static void command_show_help(struct Application *app, char * ignored) { // NOLINT(readability-non-const-parameter)
  (void)app; (void) ignored;
  for (unsigned ii = 0; ii < ARRAY_SIZE(g_commands); ++ii) {
    LOG_INFO("%s", g_commands[ii].help);
  }
  LOG_INFO0("To send a message: handle#fingerprint <your message here>");
}

static void handle_eof(struct Application *app) {
  rl_callback_handler_remove(); // avoid extra output
  if (event_base_loopexit(app->base, 0) == -1) {
    LOG_ERROR0("Cannot exit loop?!");
  }
}

static void handle_command(struct Application *app, char * line) {
  int found = 0;
  for (unsigned ii = 0; ii < ARRAY_SIZE(g_commands); ++ii) {
    size_t size = strlen(g_commands[ii].command);
    if (memcmp(g_commands[ii].command, line, size) == 0) {
      g_commands[ii].callback(app, line + size);
      found = 1;
      break;
    }
  }
  if (!found) {
    command_show_help(app, 0);
  }
}

static void handle_message(struct Application *app, char * line) {
  // Otherewise assume we have a message, where the format needs to be
  // handle[#fingerprint] <message here>
  size_t length = strlen(line);
  char *ignored = 0;
  char *peer = strtok_r(line, " ", &ignored);
  size_t length_peer = strlen(peer);
  // No message
  if (length_peer == length) {
    command_show_help(app, 0);
  } else {
    char *message = line + length_peer + 1;
    assert(*message != 0);
    if (peer_send_message(app->fingerprint, peer, message, app->peers,
                          app->num_peers, app_ack_message_cb, app) == -1) {
      LOG_ERROR0("Unable to send message");
    }
  }
}

static void readline_handler(char *line) {
  assert(g_app_readline != 0);
  if (line == 0)
    handle_eof(g_app_readline);
  else if (*line == '/')
    handle_command(g_app_readline, line);
  else if (strlen(line))
    handle_message(g_app_readline, line);
  free(line);
}

static void stdin_callback(evutil_socket_t socket, short flags, void *app) {
  (void)socket;
  g_app_readline = (struct Application *)app;
  if (flags & EV_READ) { // NOLINT(hicpp-signed-bitwise)
    rl_callback_read_char();
  }
}

#define MAX_PROMPT_SIZE 256

static void app_update_prompt(struct Application *app) {
  char prompt[MAX_PROMPT_SIZE] = {0};
  (void)snprintf(prompt, ARRAY_SIZE(prompt) - 1, "P2PCHAT:%s#%d@%s> ", app->handle,
                 app->fingerprint, app->address);
  rl_callback_handler_install(prompt, &readline_handler);
}

static void app_set_handle(struct Application *app, char *handle) {
  if (!handle || *handle == 0) {
    LOG_WARNING0("Attempted to set null handle, ignored");
    return;
  }
  size_t size = strlen(handle) + 1;
  free(app->handle);
  app->handle = malloc(size);
  (void)strncpy(app->handle, handle, size);
  assert(app->handle[size - 1] == 0);
  app_update_prompt(app);

  peers_notify_new_handle(app->handle, app->fingerprint, app->peers,
                          app->num_peers);
}

static int app_setup_prompt(struct Application *app,
                            struct event *event_stdin) {
  LOG_INFO0("Type /help to get started");
  if (event_assign(event_stdin, app->base, fileno(stdin), EV_READ | EV_PERSIST, // NOLINT(hicpp-signed-bitwise)
                   stdin_callback, app) == -1)
    goto failure;
  if (event_add(event_stdin, 0) == -1)
    goto failure;

  app_update_prompt(app);

  return 0;
failure:
  return -1;
}

static void app_cleanup_prompt(struct Application *app,
                               struct event *event_stdin) {
  (void)app;
  if (event_del(event_stdin) == -1) {
    LOG_ERROR0("Unable to remove event");
  }
  rl_callback_handler_remove();
}
