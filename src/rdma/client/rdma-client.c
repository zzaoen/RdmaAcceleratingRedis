#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include "get_clock.h"
#include <sys/stat.h>  
#include <hiredis/hiredis.h>

#define ARR_LEN 1024 * 1024 * 4

// char buf[ARR_LEN]; 
// char *s = buf;

struct message {
  enum {
	MSG_OFFSET,
    MSG_MAPPING_TABLE_ADD /*for mapping table*/
  }type;
  
  union {
    struct ibv_mr mr;
	unsigned long offset;
  }data;
};

struct context {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;
  unsigned long *mapping_tabel_start;/*Added to support read file by offset*/
  pthread_t cq_poller_thread;
};

const int TIMEOUT_IN_MS = 500; /* ms */
const int data_size = 1024*1024*1024;// * 1024 * 1024; /*1GB*/
const int block_size = 1024*1024*4;
const int max_mapping_table_size = 256 * 8;
const int key_number = 256;
int now_mapping_table_num = 0;
int need_mapping_table_num = 0;
long offset_num;
unsigned long *offset;
long add = -1;
long step = 1;
long loop = 5000;
long k = 0;
long recv_mapping_table_count = 0;

clock_t time_start, time_end;

cycles_t start,end;
cycles_t read_mapping_table_start[10000],read_mapping_table_end[10000];
cycles_t read_data_start[10000],read_data_end[10000];
double read_mapping_table_all = 0,read_mapping_table_ave = 0;
double read_data_all = 0,read_data_ave = 0;

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

struct connection_client {
  struct rdma_cm_id *id;
  struct ibv_qp *qp;
  int connected;

  /*These are used to recv info from server*/
  struct ibv_mr *rdma_local_mr;
  struct message *recv_msg;
  struct ibv_mr *recv_mr;
  char *rdma_local_region;
  struct ibv_mr server_mr;/*The mr of server*/

  /*Added to support mapping table*/
  struct message *send_msg; /*Send offset to server*/
  struct ibv_mr *send_mr;

  enum {
    CS_POST_SEND_ADD,
    CS_READ_TABLE,
    CS_HAS_TABLE,
    CS_HAS_DATA,
	  CS_DONE
  } client_state;
};

int max_pri(long num);
void create_offset();
int on_addr_resolved(struct rdma_cm_id *id);
int on_connection_client(struct rdma_cm_id *id);
int on_disconnect_client(struct rdma_cm_id *id);
int on_event(struct rdma_cm_event *event);
int on_route_resolved(struct rdma_cm_id *id);
void usage(const char *argv0);
void *poll_cq(void *context);
void destroy_connection_client(void *context);
void on_connect_client(void *context);
void post_send_mapping_table_add(struct connection_client *conn,long offset);

static struct context *s_ctx = NULL;

cycles_t start, end;
double cycles_to_units, sum_of_test_cycles;
int max_pri(long num) {
  int i,j;
  int flag = 0;
  for(i = num; i > 1; i--){
    j = 2;
    while(j <= i){
      if(i % j == 0) break;
        j++;
        if(j == i) flag = 1;
      }
      if(flag == 1) break;
  }

  return i;
}

void create_offset() {
  /*offset_num = max_pri(data_size / block_size);
  offset = (unsigned long *)malloc(sizeof(unsigned long) * offset_num);
  for(long i = 0; i < offset_num; i++){
    add+=step;
    while(add >= offset_num) add-=offset_num;
    offset[i] = add;
  }*/
  offset_num = data_size / block_size;
  offset = (unsigned long *)malloc(sizeof(unsigned long) * offset_num);
  for(long i = 0; i < offset_num; i++) {
    offset[i] = i;
  }
}

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

void post_receives(struct connection_client *conn)
{
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
  //printf("In %s,%s\n", __FILE__, __FUNCTION__);
}

void post_receive_mapp_add(struct connection_client *conn)
{
  post_receives(conn);
}

int on_connection_client(struct rdma_cm_id *id)
{
  on_connect_client(id->context);
  //printf("\nnum:%ld,offset:%ld,need read a new mapping table\n",k+1,offset[k]);
  post_send_mapping_table_add((struct connection_client *)id->context,offset[k]);
  now_mapping_table_num = offset[k] / (max_mapping_table_size / 8);
  //printf("In %s\n",__FUNCTION__);
  return 0;
}

void register_memory_client(struct connection_client *conn)
{
  conn->recv_msg = malloc(sizeof(struct message));
  conn->rdma_local_region = malloc(data_size + max_mapping_table_size); /*Used to store all data contents from server, using mapping table*/
  memset(conn->rdma_local_region, 0, data_size + max_mapping_table_size);
  memset(conn->rdma_local_region + max_mapping_table_size, 1, data_size);
  TEST_Z(conn->recv_mr = ibv_reg_mr(s_ctx->pd, conn->recv_msg, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE));
  TEST_Z(conn->rdma_local_mr = ibv_reg_mr(s_ctx->pd, conn->rdma_local_region, (data_size + max_mapping_table_size), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));

  conn->send_msg = malloc(sizeof(struct message));
  TEST_Z(conn->send_mr = ibv_reg_mr(s_ctx->pd, conn->send_msg, sizeof(struct message), IBV_ACCESS_LOCAL_WRITE));
}

void build_qp_attr_client(struct ibv_qp_init_attr *qp_attr)
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

void build_params_client(struct rdma_conn_param *params)
{
  memset(params, 0, sizeof(*params));

  params->initiator_depth = params->responder_resources = 1;
  params->rnr_retry_count = 7;
}

void build_context_client(struct ibv_context *verbs)
{
  //printf("In %s,%s\n", __FILE__, __FUNCTION__);
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

void build_connection_client(struct rdma_cm_id *id)
{
  //printf("In %s,%s\n", __FILE__, __FUNCTION__);
  struct connection_client *conn;
  struct ibv_qp_init_attr qp_attr;

  build_context_client(id->verbs);
  build_qp_attr_client(&qp_attr);
  TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));
  id->context = conn = (struct connection_client *)malloc(sizeof(struct connection_client));
  conn->id = id;
  conn->qp = id->qp;
  conn->connected = 0;
  register_memory_client(conn);
}

void send_message(struct connection_client *conn)
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

void post_send_mapping_table_add(struct connection_client *conn,long offset) {
  memset(conn->send_msg, 0 , sizeof(struct message));
  conn->send_msg->type = MSG_OFFSET;
  conn->send_msg->data.offset = offset;
  send_message(conn);
  conn->client_state = CS_POST_SEND_ADD;
}

int main(int argc, char **argv)
{
  struct addrinfo *addr;
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *conn= NULL;
  struct rdma_event_channel *ec = NULL;

  if (argc != 3)
    usage(argv[0]);

  TEST_NZ(getaddrinfo(argv[1], "12345", NULL, &addr));

  TEST_Z(ec = rdma_create_event_channel());
  TEST_NZ(rdma_create_id(ec, &conn, NULL, RDMA_PS_TCP));
  TEST_NZ(rdma_resolve_addr(conn, NULL, addr->ai_addr, TIMEOUT_IN_MS));

  freeaddrinfo(addr);

  create_offset();

  while (rdma_get_cm_event(ec, &event) == 0) {
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);

    if (on_event(&event_copy))
      break;
  }

  rdma_destroy_event_channel(ec);

  return 0;
}

int on_addr_resolved(struct rdma_cm_id *id)
{
  //printf("address resolved. pid:%d\n", getpid());

  build_connection_client(id);

  TEST_NZ(rdma_resolve_route(id, TIMEOUT_IN_MS));

  return 0;
}

void on_connect_client(void *context)
{
  ((struct connection_client *)context)->connected = 1;
}


int on_disconnect(struct rdma_cm_id *id)
{
  printf("disconnected.\n");

  destroy_connection_client(id->context);
  return 1; /* exit event loop */
}


void destroy_connection_client(void *context)
{
  struct connection_client *conn = (struct connection_client *)context;
  
  rdma_destroy_qp(conn->id);
  ibv_dereg_mr(conn->recv_mr);
  ibv_dereg_mr(conn->rdma_local_mr);

  free(conn->recv_msg);
  free(conn->rdma_local_region);

  rdma_destroy_id(conn->id);

  free(conn);
}

int on_disconnect_client(struct rdma_cm_id *id)
{
  printf("peer disconnected.\n");

  destroy_connection_client(id->context);
  return 0;
}


int on_event(struct rdma_cm_event *event)
{
  int r = 0;

  if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED)
    r = on_addr_resolved(event->id);
  else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED)
    r = on_route_resolved(event->id);
  else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
    r = on_connection_client(event->id);
  else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
    r = on_disconnect_client(event->id);
  else
    die("on_event: unknown event.");

  return r;
}

int on_route_resolved(struct rdma_cm_id *id)
{
  struct rdma_conn_param cm_params;

  //printf("route resolved.\n");
  build_params_client(&cm_params);
  TEST_NZ(rdma_connect(id, &cm_params));

  return 0;
}

void usage(const char *argv0)
{
  fprintf(stderr, "usage: %s <server-address> <server-port> \n", argv0);
  exit(1);
}

void post_send_rdma_read_mp(struct connection_client *conn){
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));
  wr.wr_id = (uintptr_t)conn;
  wr.opcode = IBV_WR_RDMA_READ;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = (uintptr_t)conn->server_mr.addr;
  wr.wr.rdma.rkey = conn->server_mr.rkey;

  sge.addr = (uintptr_t)conn->rdma_local_region;
  sge.length = max_mapping_table_size;
  sge.lkey = conn->rdma_local_mr->lkey;

  TEST_NZ(ibv_post_send(conn->qp, &wr, &bad_wr));
  //rdma_post_read(conn->id,s_ctx->ctx, conn->rdma_local_region, max_mapping_table_size,
	//conn->rdma_local_mr, 1, (uintptr_t)(conn->server_mr.addr + mapping_num * max_mapping_table_size), conn->server_mr.rkey);
}

void post_send_rdma_read_data(struct connection_client *conn,int offset)
{
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));
  wr.wr_id = (uintptr_t)conn;
  wr.opcode = IBV_WR_RDMA_READ;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = (uintptr_t)(*((unsigned long *)conn->rdma_local_region +  offset));
  wr.wr.rdma.rkey = conn->server_mr.rkey;

  sge.addr = (uintptr_t)(conn->rdma_local_region + max_mapping_table_size + k * block_size);
  sge.length = block_size;
  sge.lkey = conn->rdma_local_mr->lkey;
  
  TEST_NZ(ibv_post_send(conn->qp, &wr, &bad_wr));
}

void on_completion_client(struct ibv_wc *wc, redisContext* redis_conn)
{

  // redisContext* redis_conn = redisConnect("127.0.0.1", 6379);  
  // if(redis_conn->err)   printf("connection error:%s\n", redis_conn->errstr); 

  struct connection_client *conn = (struct connection_client *)(uintptr_t)wc->wr_id;

  if(wc->status != IBV_WC_SUCCESS) 
    die("not success wc");

  if (wc->opcode & IBV_WC_RECV) {
    if(conn->recv_msg->type == MSG_MAPPING_TABLE_ADD){
      //printf("recv mapping table address success\n");
      memcpy(&conn->server_mr, &conn->recv_msg->data.mr, sizeof(conn->server_mr));
      //printf("add of mapping table add:%p\n\n",conn->server_mr.addr);
      
      read_mapping_table_start[recv_mapping_table_count] = get_cycles();
      start =  get_cycles();

      // time_start = clock();

      post_send_rdma_read_mp(conn);
      conn->client_state = CS_READ_TABLE;
    }
  }
  else { /*the send_wr has completed, we can read data now*/
    if(conn->client_state == CS_POST_SEND_ADD) {
      post_receive_mapp_add(conn);
    } 
    if(conn->client_state == CS_READ_TABLE) {
      printf("rdma read number %d mapping table success\n",now_mapping_table_num+1);
      conn->client_state = CS_HAS_TABLE;
      
      unsigned long *p = (unsigned long *)conn->rdma_local_region;
      for(int i = 0; i < max_mapping_table_size / 8; i++) {
        p++;
      }
    }
    if(conn->client_state == CS_HAS_TABLE) {

      time_start = clock();
      post_send_rdma_read_data(conn,offset[k]);
      conn->client_state = CS_HAS_DATA;
    }
    if(conn->client_state == CS_HAS_DATA) {    
      if(k < key_number) {
        k++;
        post_send_rdma_read_data(conn,offset[k]);
        conn->client_state = CS_HAS_DATA;
      } else{
        
	      conn->client_state = CS_DONE;
        
		    char *s=(char *)malloc(ARR_LEN);
        int i = ARR_LEN;
        int j = 0;
        
	      for(int index=0; index<key_number; index++){
          char *dis=conn->rdma_local_region + max_mapping_table_size + index * block_size;
          while(j < i){
            s[j]=(unsigned char)*((dis + j));
            j++;
          }
          redisCommand(redis_conn, "set %d %s", index, s); 
	      }
        
        time_end = clock();
        double duration = (double)(time_end - time_start) / CLOCKS_PER_SEC;
        printf("\nsync done! time:%fs\n", duration);

        destroy_connection_client(conn);
        exit(-1);
      }
      
    }
  }
}

void *poll_cq(void *context)
{
  struct ibv_cq *cq;
  struct ibv_wc wc;
  redisContext* redis_conn = redisConnect("127.0.0.1", 6379);  
  if(redis_conn->err)   printf("connection error:%s\n", redis_conn->errstr); 

  while(1) {
    TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &context));
    ibv_ack_cq_events(cq, 1);
    TEST_NZ(ibv_req_notify_cq(cq, 0));

    while(ibv_poll_cq(cq, 1, &wc))
     on_completion_client(&wc, redis_conn);
    }

  return NULL;
}
