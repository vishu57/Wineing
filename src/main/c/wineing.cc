/*
 * wineing.win.cc
 *
 * Wining hides nxcore behind a protobuf based interface. Information
 * is exchanged over ZMQ.
 *
 * Wining provides two ZMQ channels for communication:
 *
 * - Control channel, a synchronous ZMQ socket(req/rep). The client
 *   application is supposed to connect to the channel with ZMQ's REQ
 *   socket option.
 *
 * - Market data channel: An asynchronous publish/subscribe
 *   channel. All the market data will be pushed to the client through
 *   this channel. The client is expected to connect with a ZMQ SUB
 *   socket. Client better be ready!
 */

/*
 * http://www.winehq.org/docs/winedev-guide/x2800
 */

// Windows header must the first include as otherwise errors will be
// generated by the compiler that some types have already been defined
//#include <windows.h>

#include <unistd.h>
#include <pthread.h>
#include <sstream>

#include "wineing.h"
#include "core/chan.h"
#include "core/wininf.win.h"

// Google Protobuf generated headers
#include "WineingCtrlProto.pb.h"
#include "WineingMarketDataProto.pb.h"
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

#include "core/lazy.h"

/**
 * Data shared among threads. This is mainly for signalling purposes
 * but also to exchange other data. To assure each thread sees the
 * correct values it is required to follow a few rules:
 *
 * 1) to update the value use *lazy_update_gloabl_if_owner*
 * 2) to read the value only with *lazy_update_local_if_changed*
 *
 * There's a plethora of details on the topic in lazy.h
 */
static w_ctrl g_data = {
  WINEING_CTRL_CMD_INIT,
  new char[WINEING_CTRL_DEFAULT_DATA_SIZE],
  0
};

/**
 * Used to make the *market_thread* sleep if the user has either not
 * started streaming or stopped it.
 */
static pthread_mutex_t g_market_sync_mutex;
static pthread_cond_t  g_market_sync_cond;

void wineing_init(w_ctx &ctx)
{
  log(LOG_INFO, "Initializing wineing");

  lazy_init();

  // Verify the version of the library we linked against is compatible
  // with the version of the headers generated.
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  // Load the nxcore dll.
  if(0 > wininf_nxcore_load()) {
    log(LOG_ERROR, "Failed loading NxCore dll");
    exit(1);
  }

  pthread_mutex_init(&g_market_sync_mutex, NULL);
  pthread_cond_init(&g_market_sync_cond, NULL);
}

void wineing_run(w_ctx &ctx)
{
  // Good tutorial on posix threads
  // http://www.yolinux.com/TUTORIALS/LinuxTutorialPosixThreads.html
  pthread_t cchan_out_t;
  pthread_t cchan_in_t;
  pthread_t market_t;

  // The cchan_in_thread listens for incomming messages on cchan_in
  // managing the mchan_thread (market data channel) as requested by
  // the client.
  pthread_create(&cchan_out_t, NULL, cchan_out_thread, (void*)&ctx);
  pthread_create(&cchan_in_t, NULL, cchan_in_thread, (void*)&ctx);
  pthread_create(&market_t, NULL, market_thread, (void*)&ctx);

  // Wait for threads to finish
  pthread_join(cchan_out_t, NULL);
  pthread_join(cchan_in_t, NULL);
  pthread_join(market_t, NULL);
}

void wineing_shutdown(w_ctx &ctx)
{
  log(LOG_INFO, "Shutting down...");

  chan_shutdown();

  pthread_cond_destroy(&g_market_sync_cond);
  pthread_mutex_destroy(&g_market_sync_mutex);

  wininf_nxcore_free();

  // Free any protobuf specific resources
  google::protobuf::ShutdownProtobufLibrary();

  lazy_destroy();
}

/**
 * Used by *chan_recv* to parse incoming data.
 *
 * \return 0 if successfull, -1 otherwise
 */
int _recv_ctrl(void *data, size_t size, void *obj)
{
  WineingCtrlProto::Request *r =
    (WineingCtrlProto::Request*) obj;

  google::protobuf::io::ArrayInputStream is (data, size);

  bool p = r->ParseFromZeroCopyStream(&is);

  if(!p) {
    log(LOG_ERROR, "Failed parsing message");
    return -1;
  }

  return 0;
}

/**
 * Called by *chan_send* if it is safe to delete the data, that is it
 * has been successfully transferred to the socket.
 */
void _send_free(void *buffer, void *hint)
{
  delete [] (char*)buffer;
}

static inline void t_to_g (const void *t, void *g)
{
  const w_ctrl *local = (const w_ctrl*)t;
  w_ctrl *shared = (w_ctrl*)g;

  shared->size = local->size;
  if(0 < local->size) {
    memcpy(shared->data, local->data, local->size);
  }
}

static inline void g_to_t(void *t, const void *g)
{
  w_ctrl *local = (w_ctrl*)t;
  const w_ctrl *shared = (const w_ctrl*)g;

  local->size = shared->size;
  if(0 < shared->size) {
    memcpy(local->data, shared->data, shared->size);
  }
}

/**
 * The controlling thread. It waits for the client to send control
 * messages to Wineing.
 */
void* cchan_in_thread(void *_ctx)
{
  using namespace WineingCtrlProto;

  w_ctx *ctx = (w_ctx *)_ctx;
  chan *ichan_out;
  chan *cchan_in;
  char *buffer;
  // Statically allocate variables to improve runtime performance
  static Request req;
  static Response res;
    static int t_version = DEFAULTS_SHARED_VERSION_INIT;
  static w_ctrl t_data = {
    WINEING_CTRL_CMD_INIT,
    new char[WINEING_CTRL_DEFAULT_DATA_SIZE],
    0
  };

  log(LOG_INFO, "Initializing cchan_in thread (%s)",
      ctx->conf->cchan_in_fqcn);

  // Initialize inbound channel
  cchan_in = chan_init(ctx->conf->cchan_in_fqcn, CHAN_TYPE_PULL_BIND);
  if(chan_bind(cchan_in) < 0) {
    log(LOG_ERROR, "Failed binding to cchan_in (%s). Error [%s]",
        ctx->conf->cchan_in_fqcn,
        chan_error());
    return NULL;
  }

  // We can not bind to the inproc channel unless it's been created.
  ichan_out = chan_init(DEFAULTS_ICHAN_NAME, CHAN_TYPE_PUSH_CONNECT);
  while(0 > chan_bind(ichan_out)) {
    sleep(1);
  }

  log(LOG_DEBUG, "Ready to accept client requests");

  int running = 1;
  while (running) {
    std::stringstream err;
    std::stringstream tape;

    // Clear all objects (they're statically allocated and the data
    // needs to be cleared)
    req.Clear();
    res.Clear();

    // Receive data
    int rc = chan_recv(cchan_in, _recv_ctrl, &req);
    log(LOG_DEBUG,
        "Received control request [id: %li, type: %d]",
        req.requestid(),
        req.type());

    if(rc > 0) {

      // Assume we will respond with an OK. Response::ERR is only set
      // in case one happens
      res.set_requestid(req.requestid());
      res.set_type(Response::OK);

      // Process control message
      switch(req.type())
        {
        case Request::START:
          // Check that the market data thread is not already
          // started. If so send an error back to the client.
          if(t_data.cmd == WINEING_CTRL_CMD_MARKET_RUN) {
            err << "Already in RUNNING state.";
            res.set_type(Response::ERR_MARKET_RUNNING);
            res.set_err_text(err.str());
            break;
          }

          t_data.size = 0;

          // If the tape file is empty or NULL NnXcore will start
          // streaming real-time data. Make sure NxCoreAccess is
          // running and connected to the NxCore servers. See
          // http://nxcoreapi.com/doc/concept_Introduction.html.
          if(req.has_tape_file()) {
            tape << ctx->conf->tape_basedir \
                 << req.tape_file();

            // Checks whether a file exists the windows way. Remember
            // we are loading the file with NxCore which is, well,
            // Windows.
            if(0 > wininf_file_exists(tape.str().c_str())) {
              err << "File '" << tape.str() << "' not found.";
              res.set_type(Response::ERR);
              res.set_err_text(err.str());
              log(LOG_DEBUG, err.str().c_str());
              break;
            }

            // String length plus NULL byte
            t_data.size = tape.str().length() + 1;
            memcpy(t_data.data,
                   tape.str().c_str(),
                   t_data.size);
          }

          // Update g_data
          t_data.cmd = WINEING_CTRL_CMD_MARKET_RUN;
          t_version = lazy_update_global_if_owner(t_version,
                                                  &t_data,
                                                  &g_data,
                                                  t_to_g);

          pthread_mutex_lock( &g_market_sync_mutex );
          pthread_cond_signal( &g_market_sync_cond );
          pthread_mutex_unlock( &g_market_sync_mutex );
          break;

        case Request::STOP:
          t_data.cmd = WINEING_CTRL_CMD_MARKET_STOP;
          t_version = lazy_update_global_if_owner(t_version,
                                                  &t_data,
                                                  &g_data,
                                                  t_to_g);
          break;

        case Request::SHUTDOWN:
          t_data.cmd = WINEING_CTRL_CMD_SHUTDOWN;
          t_version = lazy_update_global_if_owner(t_version,
                                                  &t_data,
                                                  &g_data,
                                                  t_to_g);
          running = 0;
          break;
        }

      int buf_size = res.ByteSize();
      buffer = new char[buf_size];
      google::protobuf::io::ArrayOutputStream os (buffer, buf_size);
      res.SerializeToZeroCopyStream(&os);
      log(LOG_DEBUG, "Sending Response [id: %li, type: %i]",
          res.requestid(),
          res.type()
          );
      if(0 > chan_send(ichan_out, buffer, buf_size, _send_free)) {
        log(LOG_WARN,
            "Failed sending message to inproc channel (%s). Error %s",
            DEFAULTS_ICHAN_NAME,
            chan_error());
      }
    }
  }

  chan_destroy(cchan_in);
  chan_destroy(ichan_out);

  return NULL;
}

/**
 * The thread processing NxCore messages.
 */
void* market_thread(void *_ctx)
{
  w_ctx *ctx = (w_ctx *)_ctx;
  chan *mchan;
  chan *ichan_out;
  // Thread local version of the shared state
  static int t_version = DEFAULTS_SHARED_VERSION_INIT;
  static w_ctrl t_data = {
    WINEING_CTRL_CMD_INIT,
    new char[WINEING_CTRL_DEFAULT_DATA_SIZE],
    0
  };

  log(LOG_INFO, "Initializing market data thread (%s)",
      ctx->conf->mchan_fqcn);

  mchan = chan_init(ctx->conf->mchan_fqcn, CHAN_TYPE_PUB);
  if(0 > chan_bind(mchan)) {
    log(LOG_ERROR, "Failed binding mchan (%s). Error [%s]",
        ctx->conf->mchan_fqcn,
        chan_error());
    return NULL;
  }

  // We can not bind to the inproc channel unless it's been created.
  ichan_out = chan_init(DEFAULTS_ICHAN_NAME, CHAN_TYPE_PUSH_CONNECT);
  while(0 > chan_bind(ichan_out)) {
    sleep(1);
  }

  while(1) {
    // If WINEING_CTRL_CMD_MARKET_STOP was requested we block until
    // START is requested. The *cchan_in* thread will notify us.
    pthread_mutex_lock( &g_market_sync_mutex );
    pthread_cond_wait( &g_market_sync_cond, &g_market_sync_mutex );
    pthread_mutex_unlock( &g_market_sync_mutex );

    // NxCore callback will return upon successfully completing a tape
    // (day) but is ready to start again immediately thus the inner
    // while loop. Again the read on g_msg.ctrl is unsynchronized.
    while(1) {
      t_version = lazy_update_local_if_changed(t_version,
                                               &t_data,
                                               &g_data,
                                               g_to_t);
      log(LOG_DEBUG, "Loading nxcore tape [%s]",
          t_data.size == 0 ? "real-time" : t_data.data);

      if(t_data.cmd == WINEING_CTRL_CMD_MARKET_RUN) {
        log(LOG_DEBUG, " ... running nxcore");
        wininf_nxcore_run(ichan_out, mchan, t_data.data);
      }

      // Loop as long as no shutdown is requested (g_msg.ctrl == 0)
      if(t_data.cmd == WINEING_CTRL_CMD_SHUTDOWN) {
        break;
      }
    }
  }
  chan_destroy(mchan);
  chan_destroy(ichan_out);

  return NULL;
}

/**
 * Used by cchan_out_thread. Allocates a buffer of size *size*.
 */
int _ichan_out_in_copy(void *buffer, size_t size, void *out_buffer)
{
  memcpy(out_buffer, buffer, size);
  return 0;
}

void* cchan_out_thread(void *_ctx)
{
  using namespace WineingCtrlProto;

  w_ctx *ctx = (w_ctx *)_ctx;
  chan *cchan_out;
  chan *cchan_in_mem;
  int read;
  int t_version = DEFAULTS_SHARED_VERSION_INIT;
  static w_ctrl t_data = {
    WINEING_CTRL_CMD_INIT,
    new char[WINEING_CTRL_DEFAULT_DATA_SIZE],
    0
  };
  char *buffer;

  log(LOG_INFO, "Initializing cchan_out thread (%s)",
      ctx->conf->cchan_out_fqcn);

  log(LOG_WARN, "TODO: CCHAN_OUT in-mem channel buffer size is 1024.");

  // This is where we send Response messages to the
  // client(s). ZMQ_PUSH is a fan-out type socket.
  cchan_out = chan_init(ctx->conf->cchan_out_fqcn, CHAN_TYPE_PUB);
  if(0 > chan_bind(cchan_out)) {
    log(LOG_ERROR, "Failed binding cchan_out (%s). Error [%s]",
        ctx->conf->cchan_out_fqcn,
        chan_error());
    return NULL;
  }

  // This initializes the inbound memory channel where we get Response
  // messages required to be sent to the client.
  cchan_in_mem = chan_init(DEFAULTS_ICHAN_NAME, CHAN_TYPE_PULL_BIND);
  if(0 > chan_bind(cchan_in_mem)) {
    log(LOG_ERROR, "Failed binding to cchan_in_mem (%s). Error [%s]",
        DEFAULTS_ICHAN_NAME,
        chan_error());
    return NULL;
  }

  while(1) {
    // Check whether shutdown was requested.
    t_version = lazy_update_local_if_changed(t_version,
                                             &t_data,
                                             &g_data,
                                             g_to_t);
    if(t_data.cmd == WINEING_CTRL_CMD_SHUTDOWN) {
      break;
    }

    // Allocate buffer and receive the data. The buffer is freed in
    // _cchan_out_send_free (invoked by chan_send as soon as the data
    // is on the wire.
    buffer = new char[1024];
    read = chan_recv(cchan_in_mem,
                     _ichan_out_in_copy,
                     buffer);
    if(0 > read) {
      log(LOG_WARN,
          "Failed reading message from inproc channel (%s). Error %s",
          DEFAULTS_ICHAN_NAME,
          chan_error());
      continue;
    }

    // Send the data
    read = chan_send(cchan_out,
                      buffer,
                      read,
                     _send_free);
    if(0 > read) {
      log(LOG_WARN, "Sending control message failed. Error %s",
          chan_error());
    }
  }

  chan_destroy(cchan_in_mem);
  chan_destroy(cchan_out);

  return NULL;
}

