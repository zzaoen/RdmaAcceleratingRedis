#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <sys/stat.h>  
#include <hiredis/hiredis.h>

const char *DEFAULT_PORT = "12345";
struct message {
  enum {
    MSG_OFFSET,
    MSG_MAPPING_TABLE_ADD /*for mapping table*/
  }type;
  
  union {
    struct ibv_mr mr;
    unsigned long offset;/*added for client to tell read offset*/
  }data;
};

struct connection_server {
  struct rdma_cm_id *id;
  struct ibv_qp *qp;

  int connected;

  /*These are used for server send info to clients*/
  struct message *send_msg;
  struct ibv_mr *send_mr;
  char *rdma_remote_region;
  struct ibv_mr *rdma_remote_mr;

  /*These are used to receive required info from client*/
  struct message *recv_msg;
  struct ibv_mr *recv_mr;
};

struct context {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;
  unsigned long *mapping_tabel_start; /*start address of mapping table*/
  pthread_t cq_poller_thread;
};

static int on_connect_request(struct rdma_cm_id *id);
static int on_connection_server(struct rdma_cm_id *id);
static int on_disconnect_server(struct rdma_cm_id *id);
static int on_event(struct rdma_cm_event *event);
static void usage(const char *argv0);
void *poll_cq(void *context);
void send_message(struct connection_server *conn);
void init_mapping_table(void *addr);
void post_recv_send_mapp_add(struct connection_server *conn);
void send_mapping_table_add(struct connection_server *conn,unsigned long offset);

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

const int data_size = 1024 * 1024 * 1024;// * 1024 * 1024; /*1GB*/
const int block_size = 1024 * 1024 * 4;
//const int max_mapping_table_size = 4096;

static struct context *s_ctx = NULL;

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

void build_context_server(struct ibv_context *verbs)
{
  if(s_ctx) {
    if(s_ctx->ctx != verbs) 
      die("Error build context");

    return;
  }

  s_ctx = (struct context *)malloc(sizeof(struct context));
  s_ctx->ctx = verbs;

  TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx));
  TEST_Z(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));
  TEST_Z(s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0));
  TEST_NZ(ibv_req_notify_cq(s_ctx->cq, 0));
  TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, NULL));
}

void init_mapping_table(void *addr)
{
  int num_entries = data_size / block_size;
  unsigned long mapping_table_size = num_entries * 8;
  unsigned long data_start_addr = (unsigned long)addr + mapping_table_size;
  unsigned long *p = (unsigned long *)addr; 
 
  for (int i = 0; i < num_entries; i++) {
    *p = data_start_addr + i * block_size;
    // printf("i:%d,addr:%p,contents:%lx\n", i, (void *)p, *p);
    p++;
  }
  s_ctx->mapping_tabel_start = (unsigned long *)addr;
}

void post_recv_send_mapp_add(struct connection_server *conn){
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  wr.wr_id = (uintptr_t)conn;
  wr.next = NULL;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  sge.addr = (uintptr_t)conn->recv_msg;
  sge.length = sizeof(struct message);
  sge.lkey = conn->recv_mr->lkey;

  TEST_NZ(ibv_post_recv(conn->qp, &wr, &bad_wr));
}

void register_memory_server(struct connection_server *conn)
{
  redisContext* redis_conn = redisConnect("127.0.0.1", 6379);  
  if(redis_conn->err)   printf("connection error:%s\n", redis_conn->errstr); 
  conn->send_msg = malloc(sizeof(struct message));
  conn->rdma_remote_region = malloc(data_size + 8 * (data_size / block_size));
  memset(conn->send_msg, 0, sizeof(struct message));
  memset(conn->rdma_remote_region, 0, data_size + 8 * (data_size / block_size));
  for(int i=0;i<255;i++)
  {
    //int key=1;
    redisReply* reply = redisCommand(redis_conn, "get %d",3); 
    //printf("%s\n",reply->str);
    memcpy(conn->rdma_remote_region +i*1024*1024*4+ data_size / block_size * 8,reply->str,strlen(reply->str));
  }
  //memset(conn->rdma_remote_region + data_size / block_size * 8, 1, data_size);
  init_mapping_table(conn->rdma_remote_region);
    
  //printf("In %s, server_remote_addr:%p\n", __FUNCTION__, (void *)conn->rdma_remote_region);
  TEST_Z(conn->send_mr = ibv_reg_mr(s_ctx->pd, conn->send_msg, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));
  TEST_Z(conn->rdma_remote_mr = ibv_reg_mr(s_ctx->pd, conn->rdma_remote_region, (data_size + 8 * (data_size / block_size)), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));

  conn->recv_msg = malloc(sizeof(struct message));
  TEST_Z(conn->recv_mr = ibv_reg_mr(s_ctx->pd, conn->recv_msg, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));
}

void build_qp_attr_server(struct ibv_qp_init_attr *qp_attr)
{
  memset(qp_attr, 0, sizeof(*qp_attr));
  qp_attr->send_cq = s_ctx->cq;
  qp_attr->recv_cq = s_ctx->cq;
  qp_attr->qp_type = IBV_QPT_RC;
  qp_attr->cap.max_send_wr = 10;
  qp_attr->cap.max_recv_wr = 10;
  qp_attr->cap.max_send_sge = 1;
  qp_attr->cap.max_recv_sge = 1;
}

void build_params_server(struct rdma_conn_param *params)
{
  memset(params, 0, sizeof(*params));

  params->initiator_depth = params->responder_resources = 1;
  params->rnr_retry_count = 7;
}

void build_connection_server(struct rdma_cm_id *id)
{
  struct connection_server *conn;
  struct ibv_qp_init_attr qp_attr;

  build_context_server(id->verbs);
  build_qp_attr_server(&qp_attr);
  TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));
  id->context = conn = (struct connection_server *)malloc(sizeof(struct connection_server));
  conn->id = id;
  conn->qp = id->qp;
  conn->connected = 0;
  register_memory_server(conn);
  post_recv_send_mapp_add(conn);
}

int main(int argc, char **argv)
{
  struct sockaddr_in addr;
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *listener = NULL;
  struct rdma_event_channel *ec = NULL;
  uint16_t port = 0;

  if (argc != 1)
    usage(argv[0]);

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(12345);

  TEST_Z(ec = rdma_create_event_channel());
  TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP));
  TEST_NZ(rdma_bind_addr(listener, (struct sockaddr *)&addr));
  TEST_NZ(rdma_listen(listener, 10)); /* backlog=10 is arbitrary */

  port = ntohs(rdma_get_src_port(listener));

  printf("listening on port %d.\n", port);

  while (rdma_get_cm_event(ec, &event) == 0) {
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);

    if (on_event(&event_copy))
      break;
  }

  rdma_destroy_id(listener);
  rdma_destroy_event_channel(ec);

  return 0;
}

int on_connect_request(struct rdma_cm_id *id)
{
  struct rdma_conn_param cm_params;

  //printf("received connection request. pid:%d\n", getpid());
  build_connection_server(id);
  build_params_server(&cm_params);

  TEST_NZ(rdma_accept(id, &cm_params));

  return 0;
}

void on_connect_server(void *context)
{
  ((struct connection_server *)context)->connected = 1;
}

void send_message(struct connection_server *conn)
{
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));
  wr.wr_id = (uintptr_t)conn;
  wr.opcode = IBV_WR_SEND;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  sge.addr = (uintptr_t)conn->send_msg;
  sge.length = sizeof(struct message);
  sge.lkey = conn->send_mr->lkey;

  while(!conn->connected);

  TEST_NZ(ibv_post_send(conn->qp, &wr, &bad_wr));
  //printf("In %s,%s\n", __FILE__, __FUNCTION__);
}

void send_mapping_table_add(struct connection_server *conn,unsigned long offset){
  //long need_mapping_table_num = offset / (max_mapping_table_size / 8);
  
  conn->send_msg->type = MSG_MAPPING_TABLE_ADD;
  memcpy(&conn->send_msg->data.mr, conn->rdma_remote_mr, sizeof(struct ibv_mr));
  conn->send_msg->data.mr.addr = (void *)(conn->rdma_remote_region);
  //printf("send mapping table add:%p\n", conn->send_msg->data.mr.addr);
  send_message(conn);
}

int on_connection_server(struct rdma_cm_id *id)
{
  on_connect_server(id->context);
  //printf("In %s\n",__FUNCTION__);
  return 0;
}

void destroy_connection_server(void *context)
{
  struct connection_server *conn = (struct connection_server *)context;
  
  rdma_destroy_qp(conn->id);
  ibv_dereg_mr(conn->send_mr);
  ibv_dereg_mr(conn->rdma_remote_mr);

  free(conn->send_msg);
  free(conn->rdma_remote_region);

  rdma_destroy_id(conn->id);

  free(conn);
}

int on_disconnect_server(struct rdma_cm_id *id)
{
  printf("peer disconnected.\n");

  destroy_connection_server(id->context);
  return 0;
}

int on_event(struct rdma_cm_event *event)
{
  int r = 0;

  if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
    r = on_connect_request(event->id);
  else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
    r = on_connection_server(event->id);
  else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
    r = on_disconnect_server(event->id);
  else
    die("on_event: unknown event.");

  return r;
}

void usage(const char *argv0)
{
  fprintf(stderr, "usage: %s \n \n", argv0);
  exit(1);
}

void on_completion_server(struct ibv_wc *wc)
{
  struct connection_server *conn = (struct connection_server *)(uintptr_t)wc->wr_id;

  if(wc->status != IBV_WC_SUCCESS) 
    die("not success wc");

  if (wc->opcode & IBV_WC_RECV) {
    if(conn->recv_msg->type == MSG_OFFSET) {
      unsigned long recv_offset = conn->recv_msg->data.offset;
      //printf("\nrecv success,offset:%lu\n",recv_offset);
      send_mapping_table_add(conn,recv_offset);
    }
  } else {
    printf("send mmaping table address success\n");
  }
  
}

void *poll_cq(void *context)
{
  struct ibv_cq *cq;
  struct ibv_wc wc;

  while(1) {
    TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &context));
    ibv_ack_cq_events(cq, 1);
    TEST_NZ(ibv_req_notify_cq(cq, 0));

    while(ibv_poll_cq(cq, 1, &wc))
     on_completion_server(&wc);
    }

  return NULL;
}
