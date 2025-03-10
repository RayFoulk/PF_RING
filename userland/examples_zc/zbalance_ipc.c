/*
 * (C) 2003-20 - ntop 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <sys/inotify.h>
#include <fcntl.h>

#include "pfring.h"
#include "pfring_zc.h"
#include "pfring_mod_sysdig.h"

#include "zutils.c"

#define DEFAULT_CONF_FILE "/etc/cluster/cluster.conf"

#define bitmap64_t(name, n) u_int64_t name[n / 64]
#define bitmap64_reset(b) memset(b, 0, sizeof(b))
#define bitmap64_set_bit(b, i)   b[i >> 6] |=  ((u_int64_t) 1 << (i & 0x3F))
#define bitmap64_isset_bit(b, i) !!(b[i >> 6] &   ((u_int64_t) 1 << (i & 0x3F)))  

#ifndef VLAN_VID_MASK
#define VLAN_VID_MASK 0x0fff
#endif

#define ALARM_SLEEP             1
#define MAX_CARD_SLOTS      32768
#define PREFETCH_BUFFERS        8
#define QUEUE_LEN            8192
#define POOL_SIZE              16
#define CACHE_LINE_LEN         64
#define MAX_NUM_APP	       32
#define IN_POOL_SIZE          256

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

pfring_zc_cluster *zc;
pfring_zc_worker *zw;
pfring_zc_queue **inzqs;
pfring_zc_queue **outzqs;
pfring_zc_multi_queue *outzmq; /* fanout */
pfring_zc_buffer_pool *wsp;

u_int32_t num_devices = 0;
u_int32_t num_apps = 0;
u_int32_t num_consumer_queues = 0;
u_int32_t queue_len = QUEUE_LEN;
u_int32_t pool_size = POOL_SIZE;
u_int32_t instances_per_app[MAX_NUM_APP];
char **devices = NULL;
char **outdevs;

int gtpc_fwd_queue = -1;
int gtpc_fwd_version = 0;

int cluster_id = DEFAULT_CLUSTER_ID+4;
int metadata_len = 0;

int bind_worker_core = -1;
int bind_time_pulse_core = -1;

u_int32_t time_pulse_resolution = 0;
volatile u_int64_t *pulse_timestamp_ns;

static struct timeval start_time;
u_int8_t wait_for_packet = 1, enable_vm_support = 0, time_pulse = 0, print_interface_stats = 0, proc_stats_only = 0, daemon_mode = 0;
volatile u_int8_t do_shutdown = 0;

u_int8_t n2disk_producer = 0;
u_int32_t n2disk_threads;

char *vlan_filter = NULL;
bitmap64_t(allowed_vlans, 1024);

#define MAX_MAP_VLAN_SIZE 4
u_int8_t map_vlan_size = 0;
u_int16_t map_vlan[MAX_MAP_VLAN_SIZE];

#define ETH_P_8585         0x8585
u_int16_t eth_distr_type = ETH_P_8585;

#ifdef HAVE_PF_RING_FT
int notify_fd;
int notify_wd;
#endif

/* ******************************** */

#ifdef HAVE_PF_RING_FT
#include "pfring_ft.h"

u_int8_t flow_table = 0;
pfring_ft_table *ft = NULL;
char *ft_proto_conf = NULL;

void flow_init(pfring_ft_flow *flow, void *user) {
  // Enable this by uncommenting pfring_ft_set_new_flow_callback()
  // Here you can initialise user data in value->user
  // pfring_ft_flow_value *value = pfring_ft_flow_get_value(flow);
  // value->user = ..
}

void flow_packet_process(const u_char *data, pfring_ft_packet_metadata *metadata, pfring_ft_flow *flow, void *user) {
  // Enable this by uncommenting pfring_ft_set_flow_packet_callback
  // Here you can process the packet and set the action to discard or forward packets for this flow:
  // pfring_ft_flow_set_action(flow, PFRING_FT_ACTION_DISCARD);
}

void flow_free(pfring_ft_flow *flow, void *user) {
  // Enable this by uncommenting pfring_ft_set_flow_export_callback
  // Here you can free user data stored in value->user
  // pfring_ft_flow_value *value = pfring_ft_flow_get_value(flow);
  // free(value->user);
}

#endif

/* ******************************** */

#ifdef HAVE_ZMQ
volatile u_int32_t epoch = 0; /* (sec) */

#include "hash/inplace_hash.c"
#include "zmq/server_core.c"

char *zmq_endpoint = DEFAULT_ENDPOINT;
u_int8_t zmq_server = 0;
u_int8_t default_action = PASS;
inplace_hash_table_t *src_ip_hash = NULL;
inplace_hash_table_t *dst_ip_hash = NULL;

int zmq_filtering_rule_handler(struct filtering_rule *rule) {
  inplace_key_t key;
  u_int32_t value;
  int rc = 0;
#if 1
  char buf[64];

  trace(TRACE_DEBUG, "[ZMQ] Adding rule for %s IPv%u %s [lifetime %us]\n",
    rule->bidirectional ? "src/dst" : (rule->src_ip ? "src" : "dst"),
    rule->v4 ? 4 : 6,
    rule->v4 ? intoaV4(ntohl(rule->ip.v4), buf, sizeof(buf)) : intoaV6(&rule->ip.v6, buf, sizeof(buf)),
    rule->duration);
#endif

  if (rule->v4) {
    key.ip_version = 4;
    key.ip_address.v4.s_addr = rule->ip.v4;
  } else {
    key.ip_version = 6;
    memcpy(key.ip_address.v6.s6_addr, rule->ip.v6, sizeof(key.ip_address.v6.s6_addr));
  }

  if (rule->remove) {
    if ( rule->src_ip || rule->bidirectional) inplace_remove(src_ip_hash, &key);
    if (!rule->src_ip || rule->bidirectional) inplace_remove(dst_ip_hash, &key);
  } else {
    value = rule->action_accept ? PASS : DROP;
    if (rule->src_ip || rule->bidirectional)
      if (inplace_insert(src_ip_hash, &key, rule->duration ? epoch+rule->duration : 0x7FFFFFFF, value) < 0)
        rc = -1;
    if (!rule->src_ip || rule->bidirectional)
      if (inplace_insert(dst_ip_hash, &key, rule->duration ? epoch+rule->duration : 0x7FFFFFFF, value) < 0) 
        rc = -1;
  }

  return rc;
}

void *zmq_server_thread(void *data) {
  zmq_server_listen(zmq_endpoint, DEFAULT_ENCRYPTION_KEY, zmq_filtering_rule_handler);
  return NULL;
}

void print_filter_handler(inplace_hash_table_t *ht, inplace_item_t *item) {
  u_int32_t now = epoch;
  char buf[64];

  trace(TRACE_NORMAL, "[HT] %s IPv%u %s %s [lifetime %us]\n",
    ht == src_ip_hash ? "src" : "dst", item->key.ip_version,
    item->key.ip_version == 4 ? intoaV4(ntohl(item->key.ip_address.v4.s_addr), buf, sizeof(buf)) : 
                                intoaV6(&item->key.ip_address.v6, buf, sizeof(buf)),
    item->value == PASS ? "PASS" : "DROP",
    item->expiration > now ? (item->expiration - now) : 0
  );
}

void print_filter(int signo) {
  trace(TRACE_NORMAL, "Received signal %d: printing active rules..", signo);

  inplace_iterate(src_ip_hash, print_filter_handler);
  inplace_iterate(dst_ip_hash, print_filter_handler);
}
#endif

/* ******************************** */

#define SET_TS_FROM_PULSE(p, t) { u_int64_t __pts = t; p->ts.tv_sec = __pts >> 32; p->ts.tv_nsec = __pts & 0xffffffff; }
#define SET_TIMEVAL_FROM_PULSE(tv, t) { u_int64_t __pts = t; tv.tv_sec = __pts >> 32; tv.tv_usec = (__pts & 0xffffffff)/1000; }

void *time_pulse_thread(void *data) {
  u_int64_t ns;
  struct timespec tn;
  u_int64_t pulse_clone = 0;

  bind2core(bind_time_pulse_core);

  while (likely(!do_shutdown)) {
    /* clock_gettime takes up to 30 nsec to get the time */
    clock_gettime(CLOCK_REALTIME, &tn);

    ns = ((u_int64_t) ((u_int64_t) tn.tv_sec * 1000000000) + (tn.tv_nsec));

    if (ns >= pulse_clone + 100 /* nsec precision (avoid updating each cycle to reduce cache thrashing) */ ) {
      *pulse_timestamp_ns = ((u_int64_t) ((u_int64_t) tn.tv_sec << 32) | tn.tv_nsec);
#ifdef HAVE_ZMQ
      if (epoch != tn.tv_sec)
        epoch = tn.tv_sec;
#endif
      pulse_clone = ns;
    }

    if (ns < (pulse_clone + time_pulse_resolution) &&
        (pulse_clone + time_pulse_resolution) - ns >= 100000 /* usleep takes ~55 usec */)
      usleep(1); /* optimisation to reduce load */
  }

  return NULL;
}

/* ******************************** */

void print_stats() {
  static u_int8_t print_all = 0;
  static struct timeval last_time;
  static unsigned long long last_tot_recv = 0, last_tot_slave_sent = 0;
  //static unsigned long long last_tot_slave_recv = 0;
  static unsigned long long last_tot_drop = 0, last_tot_slave_drop = 0;
  unsigned long long tot_recv = 0, tot_drop = 0, tot_slave_sent = 0, tot_slave_recv = 0, tot_slave_drop = 0;
  struct timeval end_time;
  char buf1[64], buf2[64], buf3[64], buf4[64];
  pfring_zc_stat stats;
  char stats_buf[1024];
  char time_buf[128];
  double duration;
  int i;

  if (start_time.tv_sec == 0)
    gettimeofday(&start_time, NULL);
  else
    print_all = 1;

  gettimeofday(&end_time, NULL);

  duration = delta_time(&end_time, &start_time);

  for (i = 0; i < num_devices; i++)
    if (pfring_zc_stats(inzqs[i], &stats) == 0)
      tot_recv += stats.recv, tot_drop += stats.drop;

  if (!daemon_mode && !proc_stats_only) {
    trace(TRACE_INFO, "=========================");
    trace(TRACE_INFO, "Queue TX Stats (Packets sent to applications):");
  }
  
  for (i = 0; i < num_consumer_queues; i++)
    if (pfring_zc_stats(outzqs[i], &stats) == 0) {
      tot_slave_sent += stats.sent, tot_slave_recv += stats.recv, tot_slave_drop += stats.drop;
      
      if (!daemon_mode && !proc_stats_only)
	trace(TRACE_INFO, "                   Queue %2u: %s pkts (%s drops)\n", i,
	      pfring_format_numbers((double)stats.sent+stats.drop, buf1, sizeof(buf1), 0),
	      pfring_format_numbers((double)stats.drop, buf2, sizeof(buf2), 0));      
    }
  
  if (!daemon_mode && !proc_stats_only) {
    trace(TRACE_INFO, "");
    trace(TRACE_INFO, "Total Absolute Stats: Recv %s pkts (%s drops) - Forwarded %s pkts (%s drops)\n", 
            pfring_format_numbers((double)tot_recv, buf1, sizeof(buf1), 0),
	    pfring_format_numbers((double)tot_drop, buf2, sizeof(buf2), 0),
	    pfring_format_numbers((double)tot_slave_sent, buf3, sizeof(buf3), 0),
	    pfring_format_numbers((double)tot_slave_drop, buf4, sizeof(buf4), 0)
    );

  }

  if (print_all && last_time.tv_sec > 0) {
    double delta_msec = delta_time(&end_time, &last_time);
    unsigned long long diff_recv = tot_recv - last_tot_recv;
    unsigned long long diff_drop = tot_drop - last_tot_drop;
    unsigned long long diff_slave_sent = tot_slave_sent - last_tot_slave_sent;
    //unsigned long long diff_slave_recv = tot_slave_recv - last_tot_slave_recv;
    unsigned long long diff_slave_drop = tot_slave_drop - last_tot_slave_drop;

    if (!daemon_mode && !proc_stats_only) {
      trace(TRACE_INFO, "Actual Stats:         Recv %s pps (%s drops) - Forwarded %s pps (%s drops)\n",
	      pfring_format_numbers(((double)diff_recv/(double)(delta_msec/1000)),  buf1, sizeof(buf1), 1),
	      pfring_format_numbers(((double)diff_drop/(double)(delta_msec/1000)),  buf2, sizeof(buf2), 1),
	      pfring_format_numbers(((double)diff_slave_sent/(double)(delta_msec/1000)),  buf3, sizeof(buf3), 1),
	      pfring_format_numbers(((double)diff_slave_drop/(double)(delta_msec/1000)),  buf4, sizeof(buf4), 1)
      );
    }
  }

  snprintf(stats_buf, sizeof(stats_buf), 
           "ClusterId:    %d\n"
           "TotQueues:    %d\n"
           "Applications: %d\n", 
           cluster_id,
           num_consumer_queues,
           num_apps);

  for (i = 0; i < num_apps; i++)
    snprintf(&stats_buf[strlen(stats_buf)], sizeof(stats_buf)-strlen(stats_buf), 
             "App%dQueues:   %d\n", 
             i, instances_per_app[i]);

  snprintf(&stats_buf[strlen(stats_buf)], sizeof(stats_buf)-strlen(stats_buf),
           "Duration:     %s\n"
  	   "Packets:      %lu\n"
	   "Forwarded:    %lu\n"
	   "Processed:    %lu\n",
           msec2dhmsm(duration, time_buf, sizeof(time_buf)),
	   (long unsigned int)tot_recv,
	   (long unsigned int)tot_slave_sent,
	   (long unsigned int)tot_slave_recv);

  if (print_interface_stats) {
    int i;
    u_int64_t tot_if_recv = 0, tot_if_drop = 0;
    for (i = 0; i < num_devices; i++) {
      if (pfring_zc_stats(inzqs[i], &stats) == 0) {
        tot_if_recv += stats.recv;
        tot_if_drop += stats.drop;
        if (!daemon_mode && !proc_stats_only) {
          trace(TRACE_INFO, "                %s RX %lu pkts Dropped %lu pkts (%.1f %%)\n", 
                  devices[i], stats.recv, stats.drop, 
	          stats.recv == 0 ? 0 : ((double)(stats.drop*100)/(double)(stats.recv + stats.drop)));
        }
      }
    }
    snprintf(&stats_buf[strlen(stats_buf)], sizeof(stats_buf)-strlen(stats_buf),
             "IFPackets:    %lu\n"
  	     "IFDropped:    %lu\n",
	     (long unsigned int)tot_if_recv, 
	     (long unsigned int)tot_if_drop);

    trace(TRACE_INFO, "Queue RX Stats (Packets read by applications):");
    
    for (i = 0; i < num_consumer_queues; i++) {
      if (pfring_zc_stats(outzqs[i], &stats) == 0) {
        if (!daemon_mode && !proc_stats_only) {
          trace(TRACE_INFO, "                   Queue %2u: RX %lu pkts Dropped %lu pkts (%.1f %%)\n", 
                  i, stats.recv, stats.drop, 
	          stats.recv == 0 ? 0 : ((double)(stats.drop*100)/(double)(stats.recv + stats.drop)));
        }
        if (outdevs[i]) {
          snprintf(&stats_buf[strlen(stats_buf)], sizeof(stats_buf)-strlen(stats_buf),
             "%s-TXPackets: %lu\n"
  	     "%s-TXDropped: %lu\n",
             outdevs[i], (long unsigned int) stats.sent, 
	     outdevs[i], (long unsigned int) stats.drop);
        } else {
          snprintf(&stats_buf[strlen(stats_buf)], sizeof(stats_buf)-strlen(stats_buf),
             "Q%uPackets:    %lu\n"
  	     "Q%uDropped:    %lu\n",
             i, (long unsigned int) stats.recv, 
	     i, (long unsigned int) stats.drop);
        }
      }
    }
  }

  pfring_zc_set_proc_stats(zc, stats_buf);
  
  if (!daemon_mode && !proc_stats_only)
    trace(TRACE_INFO, "=========================\n\n");
 
  last_tot_recv = tot_recv, last_tot_slave_sent = tot_slave_sent;
  //last_tot_slave_recv = tot_slave_recv;
  last_tot_drop = tot_drop, last_tot_slave_drop = tot_slave_drop;
  last_time.tv_sec = end_time.tv_sec, last_time.tv_usec = end_time.tv_usec;
}

/* ******************************** */

void sigproc(int sig) {
  static int called = 0;
  trace(TRACE_NORMAL, "Leaving...\n");
  if (called) return; else called = 1;

  pfring_zc_kill_worker(zw);

  do_shutdown = 1;

  print_stats();

#ifdef HAVE_PF_RING_FT
  if (flow_table) {
    if (ft_proto_conf != NULL) {
      inotify_rm_watch(notify_fd, notify_wd);
      close(notify_fd);
    }
  }
#endif
}

/* *************************************** */

#define MAX_BPF_FILENAME_LEN 256
#define MAX_BPF_EXPRESSION_LEN 256

struct bpf_file_node {
  char filename[MAX_BPF_FILENAME_LEN];
  char expression[MAX_BPF_EXPRESSION_LEN];
  struct bpf_file_node* next;
};

struct bpf_file_list {
  struct bpf_file_node* head;
  int modified;
};

static void append_bpf_file_list(struct bpf_file_list* list, const char* filename) {
  struct bpf_file_node* new_node = (struct bpf_file_node*)malloc(sizeof(struct bpf_file_node));
  strcpy(new_node->filename, filename);
  new_node->expression[0] = '\0'; 
  new_node->next = NULL;
  if (list->head) {
    struct bpf_file_node* node;
    for (node = list->head; node->next; node = node->next);
    node->next = new_node;
  } else
    list->head = new_node;
}

// signal-safety function
static char* read_bpf_file(const char* filename) {
  static char buf[MAX_BPF_EXPRESSION_LEN];
  int fd;
  if ((fd = open(filename, O_RDONLY)) > 0) {
    int n;
    if ((n = read(fd, buf, sizeof(buf))) >= 0) {
      buf[n] = '\0';
      char* line_end = strpbrk(buf, "\r\n");
      if (line_end) // line trim
        *line_end = '\0';
    }
    close(fd);
    return buf;
  } else
    return NULL;
}

// signal-safety function
static void read_bpf_file_list(struct bpf_file_list* list) {
  list->modified = 0;
  struct bpf_file_node* node;
  for (node = list->head; node; node = node->next) {
    char* buf = read_bpf_file(node->filename);
    if (buf && strcmp(node->expression, buf)) {
      list->modified = 1;
      strcpy(node->expression, buf);
      trace(TRACE_NORMAL, "bpf file '%s' : '%s'\n", node->filename, node->expression);
    }
  }
}

static char** init_inzq_bpf(struct bpf_file_list* list, size_t qlen) {
  int i;
  struct bpf_file_node* node;
  char** zq_bpf;
  if (!list->head)
    return NULL;
  zq_bpf = calloc(qlen, sizeof(char*));
  // same bpf for all inzq for now
  node = list->head;
  for (i = 0; i < qlen; i++) {
    zq_bpf[i] = node->expression;
    trace(TRACE_NORMAL, "inqzs[%d] bpf file : %s\n", i, node->filename);
  }
  read_bpf_file_list(list);
  return zq_bpf;
}

static char** init_outzq_bpf(struct bpf_file_list* list, size_t qlen) {
  int i;
  struct bpf_file_node* node;
  char** zq_bpf;
  if (!list->head)
    return NULL;
  zq_bpf = calloc(qlen, sizeof(char*));
  for (node = list->head; node; node = node->next) {
    char* filename_end = strchr(node->filename, '@');
    *filename_end++ = '\0';
    if (*filename_end == '\0') {
      trace(TRACE_ERROR, "outzq number must be specified after '@'\n");
      continue;
    }
    char* queue_index = strtok(filename_end, ",");
    while (queue_index) {
      i = atoi(queue_index);
      if (i < qlen) {
        zq_bpf[i] = node->expression;
        trace(TRACE_NORMAL, "outzqs[%d] bpf file : %s\n", i, node->filename);
      } else {
        trace(TRACE_ERROR, "outzq number(%d) must be less than %d\n", i, num_consumer_queues);
        continue;
      }
      queue_index = strtok(NULL, ",");
    }
  }
  read_bpf_file_list(list);
  return zq_bpf;
}

struct bpf_file_list in_bpf_file_list = {NULL, 0};
struct bpf_file_list out_bpf_file_list = {NULL, 0};
char** inzq_bpf = NULL;
char** outzq_bpf = NULL;

void on_bpf_files_modified(int sig) {
  read_bpf_file_list(&in_bpf_file_list);
  read_bpf_file_list(&out_bpf_file_list);
}

// bpf update should be done in packet_process thread but...
// - filter_func alone is not enough because if bpf doesn't match at all, it will not be called.
//   (this is especially for inzq_bpf. outzq_bpf is evaluated after filter_func anyway.)
// - idle_func alone is not enough because if traffic is too busy, it may not be called.
// that's why we need both filter_func and idle_func, either of them is always called.

void set_inzq_bpf() {
  if (in_bpf_file_list.modified) {
    int i;
    in_bpf_file_list.modified = 0;
    for (i = 0; i < num_devices; i++) {
      if (!inzq_bpf[i])
        continue;
      pfring_zc_remove_bpf_filter(inzqs[i]);
      if (pfring_zc_set_bpf_filter(inzqs[i], inzq_bpf[i]) != 0)
        trace(TRACE_NORMAL, "inzqs[%d] bpf set error : '%s'", i, inzq_bpf[i]);
      else
        trace(TRACE_NORMAL, "inzqs[%d] bpf set : '%s'", i, inzq_bpf[i]);
    }
  }
}

void set_outzq_bpf() {
  if (out_bpf_file_list.modified) {
    int i;
    out_bpf_file_list.modified = 0;
    for (i = 0; i < num_consumer_queues; i++) {
      if (!outzq_bpf[i])
        continue;
      pfring_zc_remove_bpf_filter(outzqs[i]);
      if (pfring_zc_set_bpf_filter(outzqs[i], outzq_bpf[i]) != 0)
        trace(TRACE_NORMAL, "outzqs[%d] bpf set error : '%s'", i, outzq_bpf[i]);
      else
        trace(TRACE_NORMAL, "outzqs[%d] bpf set : '%s'", i, outzq_bpf[i]);
    }
  }
}

/* *************************************** */

void printHelp(void) {
  printf("zbalance_ipc - (C) 2014-22 ntop.org\n");
  printf("Using PFRING_ZC v.%s\n", pfring_zc_version());
  printf("A master process balancing packets to multiple consumer processes.\n\n");
  printf("Usage: zbalance_ipc -i <device> -c <cluster id> -n <num inst>\n"
	 "                 [-h] [-m <hash mode>] [-S <core id>] [-g <core_id>]\n"
	 "                 [-N <num>] [-a] [-q <len>] [-Q <sock list>] [-d] \n"
	 "                 [-D <username>] [-P <pid file>] \n\n");
  printf("-h               Print this help\n");
  printf("-i <device>      Device (comma-separated list) Note: use 'Q' as device name to create ingress sw queues\n");
  printf("-c <cluster id>  Cluster id\n");
  printf("-n <num inst>    Number of application instances\n"
         "                 In case of '-m 1' or '-m 4' it is possible to spread packets across multiple\n"
         "                 instances of multiple applications, using a comma-separated list\n");
  printf("-m <hash mode>   Hashing modes:\n"
         "                 0 - No hash: Round-Robin (default)\n"
         "                 1 - Source/Dest IP hash (or Thread-ID in case of sysdig)\n"
         "                 2 - Fan-out\n"
         "                 3 - Fan-out (1st) + Round-Robin (2nd, 3rd, ..)\n"
         "                 4 - GTP hash (Inner Source/Dest IP/Port or Seq-Num or Outer Source/Dest IP/Port)\n"
         "                 5 - GRE hash (Inner or Outer Source/Dest IP)\n"
         "                 6 - Interface X to queue X\n"
         "                 7 - VLAN ID encapsulated in Ethernet type 0x8585 (see -Y). Queue is selected based on -M. Other Ethernet types to queue 0.\n");
  printf("-r <queue>:<dev> Replace egress queue <queue> with device <dev> (multiple -r can be specified)\n");
  printf("-M <vlans>       Comma-separated list of VLANs to map VLAN to egress queues (-m 7 only)\n");
  printf("-X               Capture also TX packets (standard drivers only - not supported with ZC drivers)\n");
  printf("-Y <eth type>    Ethernet type used in -m 7. Default: %u (0x8585)\n", ntohs(ETH_P_8585));
  printf("-S <core id>     Enable Time Pulse thread and bind it to a core\n");
  printf("-R <nsec>        Time resolution (nsec) when using Time Pulse thread\n"
         "                 Note: in non-time-sensitive applications use >= 100usec to reduce cpu load\n");
  printf("-g <core id>     Bind this app to a core\n");
  printf("-q <size>        Number of slots in each consumer queue (default: %u)\n", QUEUE_LEN);
  printf("-b <size>        Number of buffers in each consumer pool (default: %u)\n", POOL_SIZE);
  printf("-w               Use hw aggregation when specifying multiple devices in -i (when supported)\n");
  printf("-W <sec>         Wait <sec> seconds before processing packets\n");
  printf("-N <num>         Producer for n2disk multi-thread (<num> threads)\n");
  printf("-a               Active packet wait\n");
  printf("-Q <sock list>   Enable VM support (comma-separated list of QEMU monitor sockets)\n");
  printf("-p               Print per-interface and per-queue absolute stats\n");
  printf("-d               Daemon mode\n");
  printf("-l <path>        Dump log messages to the specified file\n");
  printf("-D <username>    Drop privileges\n");
  printf("-P <pid file>    Write pid to the specified file (daemon mode only)\n");
  printf("-u <mountpoint>  Hugepages mount point for packet memory allocation\n");
  printf("-f <bpf file>    Set a BPF filter for input queue (this may affect the performance!)\n");
  printf("                 <bpf file> contains a single-line BPF expression which can be updated at runtime by sending a SIGUSR1\n");
  printf("                 It is possible to set a BPF filter for output queues by specifying @<queue id list> (e.g. -f bpf1@0,1,2)\n");
  printf("                 Note: this option can be specified multiple times\n");
  printf("-x <vlans>       Set a VLAN filter (comma-separated list of VLAN ID)\n");
#ifdef HAVE_PF_RING_FT
  printf("-T               Enable FT (Flow Table) support for flow filtering\n");
  printf("-C <path>        FT configuration file\n");
  printf("-O <path>        FT custom protocols (nDPI) configuration file\n");
#endif
#ifdef HAVE_ZMQ
  printf("-Z               Enable IP-based filtering with ZMQ support for dynamic rules injection\n");
  printf("-E <endpoint>    Set the ZMQ endpoint to be used with -Z (default: %s)\n", DEFAULT_ENDPOINT);
  printf("-A <policy>      Set default policy (0: drop, 1: accept) to be used with -Z (default: accept)\n");
#endif
  printf("-G <queue>:<ver> Forward GTP-C version <ver> to queue <queue> (with -m 4)\n");
  printf("-E               Debug mode\n");
  printf("-v               Verbose\n");
  exit(-1);
}

/* *************************************** */

void idle_func() {
  if (inzq_bpf)
    set_inzq_bpf();
}

/* *************************************** */

int64_t packet_filtering_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {

  if (inzq_bpf)
    set_inzq_bpf();
  if (outzq_bpf)
    set_outzq_bpf();

  if (vlan_filter) {
    int rc = 0;
    u_char *data = pfring_zc_pkt_buff_data(pkt_handle, in_queue);
    struct ethhdr *eh = (struct ethhdr*) data;
    u_int16_t eth_type = ntohs(eh->h_proto);
    u_int32_t vlan_offset = sizeof(struct ethhdr);

    while (eth_type == ETH_P_8021Q /* 802.1q (VLAN) */ && 
           (vlan_offset + sizeof(struct eth_vlan_hdr)) < pkt_handle->len) {
      struct eth_vlan_hdr *vh = (struct eth_vlan_hdr *) &data[vlan_offset];
      u_int16_t vlan_id = ntohs(vh->h_vlan_id) & VLAN_VID_MASK /* 0x0fff */;

      if (bitmap64_isset_bit(allowed_vlans, vlan_id)) {
        rc = 1;
        break;
      }

      eth_type = ntohs(vh->h_proto);
      vlan_offset += sizeof(struct eth_vlan_hdr);
    }

    if (!rc)
      return 0; /* drop */
  }

#ifdef HAVE_PF_RING_FT
  if (flow_table) {
    pfring_ft_pcap_pkthdr hdr;
    pfring_ft_ext_pkthdr ext_hdr;
    pfring_ft_action action;

    hdr.len = hdr.caplen = pkt_handle->len;
    SET_TIMEVAL_FROM_PULSE(hdr.ts, *pulse_timestamp_ns);
    ext_hdr.hash = pkt_handle->hash;

    action = pfring_ft_process(ft, pfring_zc_pkt_buff_data(pkt_handle, in_queue), &hdr, &ext_hdr);

    if (action == PFRING_FT_ACTION_DISCARD)
      return 0; /* drop */
  }
#endif

#ifdef HAVE_ZMQ
  if (zmq_server) {
    inplace_key_t src_key, dst_key;
    int action = default_action;
    if (extract_keys(pfring_zc_pkt_buff_data(pkt_handle, in_queue), &src_key, &dst_key)) {
      int rule_action = NULL_VALUE;
#if 0 /* debug */
      char sbuf[64], dbuf[64];
      trace(TRACE_DEBUG, "Processing packet from %s to %s\n",
        src_key.ip_version == 4 ? intoaV4(ntohl(src_key.ip_address.v4.s_addr), sbuf, sizeof(sbuf)) : intoaV6(&src_key.ip_address.v6, sbuf, sizeof(sbuf)),
        dst_key.ip_version == 4 ? intoaV4(ntohl(dst_key.ip_address.v4.s_addr), dbuf, sizeof(dbuf)) : intoaV6(&dst_key.ip_address.v6, dbuf, sizeof(dbuf)));
#endif
      if (src_ip_hash != NULL) {
        rule_action = inplace_lookup(src_ip_hash, &src_key);
        if (rule_action != NULL_VALUE) action = rule_action;
      }
      if (dst_ip_hash != NULL && rule_action == NULL_VALUE) {
        rule_action = inplace_lookup(dst_ip_hash, &dst_key);
        if (rule_action != NULL_VALUE) action = rule_action;
      }
    }
    if (action == DROP)
      return 0; /* drop */
  }
#endif

  return 1; /* pass */
}

/* *************************************** */

int64_t ip_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  return pfring_zc_builtin_ip_hash(pkt_handle, in_queue) % num_out_queues;
}

/* *************************************** */

int64_t gtp_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  u_int32_t hash, flags;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);

  hash = pfring_zc_builtin_gtp_hash(pkt_handle, in_queue, &flags) % num_out_queues;

  if (gtpc_fwd_version && (flags & PF_RING_ZC_BUILTIN_GTP_HASH_FLAGS_GTPC)) {
    if ((gtpc_fwd_version == 1 && (flags & PF_RING_ZC_BUILTIN_GTP_HASH_FLAGS_V1)) ||
        (gtpc_fwd_version == 2 && (flags & PF_RING_ZC_BUILTIN_GTP_HASH_FLAGS_V2)))
      hash = gtpc_fwd_queue;
  }

  return hash;
}

/* *************************************** */

int64_t gre_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  return pfring_zc_builtin_gre_hash(pkt_handle, in_queue) % num_out_queues;
}

/* *************************************** */

int64_t direct_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  u_int32_t ingress_id;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  for (ingress_id = 0; ingress_id < num_devices; ingress_id++)
    if (in_queue == inzqs[ingress_id]) break;
  return ingress_id % num_out_queues;
}

/* *************************************** */

int64_t eth_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;
  u_char *data = pfring_zc_pkt_buff_data(pkt_handle, in_queue);
  struct ethhdr *eh = (struct ethhdr*) data;
  u_int16_t eth_type = ntohs(eh->h_proto);
  int64_t idx = 0;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);

  if (eth_type == eth_distr_type) {
    u_int32_t vlan_offset = sizeof(struct ethhdr);

    idx = -1;

    /* Reforge eth type to ETH_P_8021Q */
    eh->h_proto = htons(ETH_P_8021Q);

    /* Read VLAN ID */
    if ((vlan_offset + sizeof(struct eth_vlan_hdr)) < pkt_handle->len) {
      struct eth_vlan_hdr *vh = (struct eth_vlan_hdr *) &data[vlan_offset];
      u_int16_t i, vlan_id = ntohs(vh->h_vlan_id) & VLAN_VID_MASK;

      for (i = 0; i < map_vlan_size; i++)
        if (vlan_id == map_vlan[i]) { idx = i+1; break; }
    }

    if (idx == -1) return idx; /* drop on no match */
  }

  return idx % num_out_queues;
}

/* *************************************** */

static int rr = -1;

int64_t rr_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  if (++rr == num_out_queues) rr = 0;
  return rr;
}

/* *************************************** */

int64_t sysdig_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  /* NOTE: pkt_handle->hash contains the CPU id */
  struct sysdig_event_header *ev = (struct sysdig_event_header*)pfring_zc_pkt_buff_data(pkt_handle, in_queue); 
  long num_out_queues = (long) user;

  return(ev->thread_id % num_out_queues);
}

/* *************************************** */

int64_t fo_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  return 0xffffffffffffffff; 
}

/* *************************************** */

int64_t fo_rr_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  long num_out_queues = (long) user;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);
  if (++rr == (num_out_queues - 1)) rr = 0;
  return (1 << 0 /* full traffic on 1st slave */ ) | (1 << (1 + rr) /* round-robin on other slaves */ );
}

/* *************************************** */

int64_t fo_multiapp_ip_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  int32_t i, offset = 0, app_instance, hash;
  int64_t consumers_mask = 0; 

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);

  hash = pfring_zc_builtin_ip_hash(pkt_handle, in_queue);

  for (i = 0; i < num_apps; i++) {
    app_instance = hash % instances_per_app[i];
    consumers_mask |= ((int64_t) 1 << (offset + app_instance));
    offset += instances_per_app[i];
  }

  return consumers_mask;
}

/* *************************************** */

int64_t fo_multiapp_gtp_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  int32_t i, offset = 0, app_instance, hash;
  int64_t consumers_mask = 0;
  u_int32_t flags;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);

  hash = pfring_zc_builtin_gtp_hash(pkt_handle, in_queue, &flags);

  for (i = 0; i < num_apps; i++) {
    app_instance = hash % instances_per_app[i];
    consumers_mask |= ((int64_t) 1 << (offset + app_instance));
    offset += instances_per_app[i];
  }

  if (gtpc_fwd_version) {
    consumers_mask &= ~((int64_t) 1 << gtpc_fwd_queue); /* do not balance traffic to -G queue */
    if (flags & PF_RING_ZC_BUILTIN_GTP_HASH_FLAGS_GTPC) {
      if ((gtpc_fwd_version == 1 && (flags & PF_RING_ZC_BUILTIN_GTP_HASH_FLAGS_V1)) ||
          (gtpc_fwd_version == 2 && (flags & PF_RING_ZC_BUILTIN_GTP_HASH_FLAGS_V2)))
        consumers_mask |= ((int64_t) 1 << gtpc_fwd_queue);
    }
  }

  return consumers_mask;
}

/* *************************************** */

int64_t fo_multiapp_gre_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  int32_t i, offset = 0, app_instance, hash;
  int64_t consumers_mask = 0;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);

  hash = pfring_zc_builtin_gre_hash(pkt_handle, in_queue);

  for (i = 0; i < num_apps; i++) {
    app_instance = hash % instances_per_app[i];
    consumers_mask |= ((int64_t) 1 << (offset + app_instance));
    offset += instances_per_app[i];
  }

  return consumers_mask;
}

/* *************************************** */

int64_t fo_multiapp_direct_distribution_func(pfring_zc_pkt_buff *pkt_handle, pfring_zc_queue *in_queue, void *user) {
  int32_t i, offset = 0, app_instance, ingress_id;
  int64_t consumers_mask = 0;

  if (time_pulse) SET_TS_FROM_PULSE(pkt_handle, *pulse_timestamp_ns);

  for (ingress_id = 0; ingress_id < num_devices; ingress_id++)
    if (in_queue == inzqs[ingress_id]) break;

  for (i = 0; i < num_apps; i++) {
    app_instance = ingress_id % instances_per_app[i];
    consumers_mask |= ((int64_t) 1 << (offset + app_instance));
    offset += instances_per_app[i];
  }

  return consumers_mask;
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char c;
  char *device = NULL;
  char *applications = NULL, *app, *app_pos = NULL;
  char *vm_sockets = NULL, *vm_sock; 
  long i, j, off;
  int hash_mode = 0, hw_aggregation = 0;
  int num_additional_buffers = 0;
  pthread_t time_thread;
  int rc;
  int num_real_devices = 0, num_in_queues = 0, num_outdevs = 0;
  char *pid_file = NULL;
  char *hugepages_mountpoint = NULL;
  int opt_argc;
  u_int wait_time_sec = 0;
  char **opt_argv;
  char *user = NULL;
  int num_consumer_queues_limit = 0;
  u_int32_t cluster_flags = 0;
  u_int32_t rx_open_flags = 0;
  const char *opt_string = "ab:c:dD:Ef:G:g:hi:l:m:M:n:N:pr:Q:q:P:R:S:u:wvx:Y:zW:X"
#ifdef HAVE_PF_RING_FT
    "TC:O:"
#endif
#ifdef HAVE_ZMQ 
    "A:E:Z"
#endif
  ;
#ifdef HAVE_PF_RING_FT
  char *ft_rules_conf = NULL;
#endif
#ifdef HAVE_ZMQ
  pthread_t zmq_thread;
#endif
  pfring_zc_idle_callback idle_func = NULL;
  pfring_zc_distribution_func distr_func = NULL;
  pfring_zc_filtering_func filter_func = NULL;

  start_time.tv_sec = 0;

  if (argc == 1) {
    if (load_args_from_file(DEFAULT_CONF_FILE, &opt_argc, &opt_argv) != 0) {
      trace(TRACE_ERROR, "Please specify all mandatory options via cli or configuration file (default path: %s)\n", DEFAULT_CONF_FILE);
      printHelp();
      exit(-1);
    }
  } else if ((argc == 2) && (argv[1][0] != '-')) {
    if (load_args_from_file(argv[1], &opt_argc, &opt_argv) != 0) {
      trace(TRACE_ERROR, "Unable to read config file %s\n", argv[1]);
      exit(-1);
    }
  } else {
    opt_argc = argc;
    opt_argv = argv;
  }

  while ((c = getopt(opt_argc, opt_argv, opt_string)) != '?') {
    if ((c == 255) || (c == -1)) break;

    switch (c) {
    case 'a':
      wait_for_packet = 0;
      break;
    case 'b':
      pool_size = upper_power_of_2(atoi(optarg));
      break;
    case 'c':
      cluster_id = atoi(optarg);
      break;
    case 'd':
      daemon_mode = 1;
      break;
    case 'D':
      user = strdup(optarg);
      break;
    case 'E':
      pfring_zc_debug();
      break;
    case 'f':
      if (strchr(optarg, '@'))
        append_bpf_file_list(&out_bpf_file_list, optarg);
      else
        append_bpf_file_list(&in_bpf_file_list, optarg);
      break;
    case 'g':
      bind_worker_core = atoi(optarg);
      break;
    case 'h':
      printHelp();
      break;
    case 'i':
      device = strdup(optarg);
      break;
    case 'l':
      trace_file = fopen(optarg, "w");
      if (trace_file == NULL)
        trace(TRACE_ERROR, "Unable to open log file %s", optarg);
      break;
    case 'm':
      hash_mode = atoi(optarg);
      break;
    case 'M':
      if (map_vlan_size < MAX_MAP_VLAN_SIZE) {
        map_vlan[map_vlan_size] = atoi(optarg);
        trace(TRACE_NORMAL, "Mapping VLAN %u to queue %u\n", map_vlan[map_vlan_size], map_vlan_size+1);
        map_vlan_size++;
      }
      break;
    case 'n':
      applications = strdup(optarg);
      break;
    case 'N':
      n2disk_producer = 1;
      n2disk_threads = atoi(optarg);
      break;
    case 'p':
      print_interface_stats = 1;
      break;
    case 'P':
      pid_file = strdup(optarg);
      break;
    case 'q':
      queue_len = upper_power_of_2(atoi(optarg));
      break;
    case 'Q':
      enable_vm_support = 1;
      vm_sockets = strdup(optarg);
      break;
    case 'R':
      time_pulse_resolution = atoi(optarg);
      break;
    case 'S':
      time_pulse = 1;
      bind_time_pulse_core = atoi(optarg);
      break;
    case 'u':
      if (optarg != NULL) hugepages_mountpoint = strdup(optarg);
      break;
    case 'v':
      trace_verbosity = 3;    
    break;
    case 'w':
      hw_aggregation = 1;
      break;
    case 'W':
      wait_time_sec = atoi(optarg);
      if(wait_time_sec > 10)
	wait_time_sec = 10; /* Upper limit */
      break;      
    case 'x':
      vlan_filter = strdup(optarg);
    break;
    case 'X':
      rx_open_flags |= PF_RING_ZC_DEVICE_CAPTURE_TX;
    break;
    case 'Y':
      eth_distr_type = htons(atoi(optarg));
    break;
    case 'z':
      proc_stats_only = 1;
      break;
#ifdef HAVE_PF_RING_FT
    case 'T':
      flow_table = 1;
      time_pulse = 1; /* forcing time-pulse to handle flows expiration */
    break;
    case 'C':
      ft_rules_conf = strdup(optarg);
    break;
    case 'O':
      ft_proto_conf = strdup(optarg);
    break;
#endif
#ifdef HAVE_ZMQ
    case 'A':
      if (atoi(optarg) == 0) default_action = DROP;
      else default_action = PASS;
    break;
    case 'E':
      zmq_endpoint = strdup(optarg);
    break;
    case 'Z':
      zmq_server = 1;
      time_pulse = 1; /* forcing time-pulse to handle rules expiration */
    break;
#endif
    }
  }
 
  if (device == NULL) printHelp();
  if (cluster_id < 0) printHelp();
  if (applications == NULL && hash_mode != 7) printHelp();

  if (vlan_filter
#ifdef HAVE_PF_RING_FT
      || flow_table
#endif
#ifdef HAVE_ZMQ
      || zmq_server
#endif
     )
    filter_func = packet_filtering_func;

  if (vlan_filter != NULL) {
    char *vlan;
    bitmap64_reset(allowed_vlans);
    vlan = strtok(vlan_filter, ",");
    while(vlan != NULL) {
      u_int16_t vlan_id = atoi(vlan);
      if (vlan_id < 1024) {
        trace(TRACE_NORMAL, "Allow VLAN = %u", vlan_id);
        bitmap64_set_bit(allowed_vlans, vlan_id);
      }
      vlan = strtok(NULL, ",");
    }
  }

  if (n2disk_producer) {
    if (n2disk_threads < 1) printHelp();
    metadata_len = N2DISK_METADATA;
    num_additional_buffers += (n2disk_threads * (N2DISK_CONSUMER_QUEUE_LEN + 1)) + N2DISK_PREFETCH_BUFFERS;
  }

  if (!hw_aggregation) {
    char *dev; 
    dev = strtok(device, ",");
    while(dev != NULL) {
      devices = realloc(devices, sizeof(char *) * (num_devices+1));
      devices[num_devices] = strdup(dev);
      num_devices++;
      dev = strtok(NULL, ",");
    }
  } else {
    devices = calloc(1, sizeof(char *));
    devices[0] = device;
    num_devices = 1;
  }

  if (hash_mode == 7) {
    instances_per_app[num_apps] = map_vlan_size + 1;
    num_consumer_queues += instances_per_app[num_apps]; 
    num_apps++;
  } else {
    app = strtok_r(applications, ",", &app_pos);
    while (app != NULL && num_apps < MAX_NUM_APP) {
      instances_per_app[num_apps] = atoi(app);
      if (instances_per_app[num_apps] == 0) printHelp();
      num_consumer_queues += instances_per_app[num_apps];
      num_apps++;
      app = strtok_r(NULL, ",", &app_pos);
    }
  }

  if (num_apps == 0) printHelp();
  if (num_apps > 1) {
    switch (hash_mode) {
      case 1: 
      case 4:
      case 5:
      case 6:
        num_consumer_queues_limit = 64; /* egress mask is 64 bit */
        break;
      default:
        printHelp();
        break;
    }
  }
  switch (hash_mode) {
    case 1: 
    case 3:
      num_consumer_queues_limit = 64; /* egress mask is 64 bit */
      break;
    default:
      break;
  }

  if (num_consumer_queues_limit && num_consumer_queues > num_consumer_queues_limit) { 
    trace(TRACE_ERROR, "Misconfiguration detected: you cannot use more than %d egress queues in fan-out or multi-app mode\n", num_consumer_queues_limit);
    return -1;
  }

  for (i = 0; i < num_devices; i++) {
    if (strcmp(devices[i], "Q") != 0) num_real_devices++;
    else num_in_queues++;
  }

  inzqs  = calloc(num_devices, sizeof(pfring_zc_queue *));
  outzqs = calloc(num_consumer_queues,  sizeof(pfring_zc_queue *));
  outdevs = calloc(num_consumer_queues,  sizeof(char *));

  optind = 1;
  while ((c = getopt(opt_argc, opt_argv, opt_string)) != '?') {
    int q_idx;
    char *v_ptr;
    if ((c == 255) || (c == -1)) break;
    switch (c) {
      case 'G':
        q_idx = atoi(optarg);
        if (q_idx < num_consumer_queues) {
          gtpc_fwd_queue = q_idx;
          v_ptr = strchr(optarg, ':');
          if (v_ptr != NULL) gtpc_fwd_version = atoi(&v_ptr[1]);
          trace(TRACE_NORMAL, "Forwarding GTP-C v%u to queue %u", gtpc_fwd_version, gtpc_fwd_queue);
        }
      break;
      case 'r':
        q_idx = atoi(optarg);
        if (q_idx < num_consumer_queues) {
          outdevs[q_idx] = strchr(optarg, ':');
          if (outdevs[q_idx] != NULL) outdevs[q_idx]++;
        }
      break;
    }
  }

  for (i = 0; i < num_consumer_queues; i++) 
    if (outdevs[i] != NULL) {
      num_outdevs++;
      trace(TRACE_NORMAL, "Mapping egress queue %ld to device %s\n", i, outdevs[i]);
    }

  if (daemon_mode)
    daemonize();

  cluster_flags = 0;

  if (enable_vm_support)
    cluster_flags |= PF_RING_ZC_ENABLE_VM_SUPPORT;

  zc = pfring_zc_create_cluster(
    cluster_id, 
    max_packet_len(devices[0]),
    metadata_len,
    (num_real_devices * MAX_CARD_SLOTS) + (num_in_queues * (queue_len + IN_POOL_SIZE)) 
     + (num_consumer_queues * (queue_len + pool_size)) + PREFETCH_BUFFERS + num_additional_buffers
     + (num_outdevs * MAX_CARD_SLOTS) - (num_outdevs * (queue_len /* replaced queues */ - 1 /* dummy queues */)), 
    pfring_zc_numa_get_cpu_node(bind_worker_core),
    hugepages_mountpoint,
    cluster_flags 
  );

  if (zc == NULL) {
    trace(TRACE_ERROR, "pfring_zc_create_cluster error [%s]", strerror(errno));
    switch (errno) {
      case ENOBUFS:
        trace(TRACE_ERROR, "Insufficient hugepage memory, please try increasing your hugepage count");
	trace(TRACE_ERROR, "Please see https://www.ntop.org/guides/pf_ring/hugepages.html");
      break;
      case EAFNOSUPPORT:
        trace(TRACE_ERROR, "PF_RING kernel module not loaded, please start the pfring service");
      break;
      default:
        trace(TRACE_ERROR, "Failure can be related to the hugetlb configuration");
      break;
    }
    return -1;
  }

  for (i = 0; i < num_devices; i++) {
    if (strcmp(devices[i], "Q") != 0) {

      inzqs[i] = pfring_zc_open_device(zc, devices[i], rx_only, rx_open_flags);

      if (inzqs[i] == NULL) {
        trace(TRACE_ERROR, "pfring_zc_open_device error [%s] Please check that %s is up and not already used\n",
	        strerror(errno), devices[i]);
        pfring_zc_destroy_cluster(zc);
        return -1;
      }

    } else { /* create sw queue as ingress device */
      pfring_zc_queue *ext_q = NULL;
      pfring_zc_buffer_pool *ext_pool = NULL;

      rc = pfring_zc_create_queue_pool_pair(zc, queue_len, IN_POOL_SIZE, &ext_q, &ext_pool);

      if (rc < 0 || ext_q == NULL || ext_pool == NULL) {
        trace(TRACE_ERROR, "pfring_zc_create_queue_pool_pair error [%s]\n", strerror(errno));                                             
        pfring_zc_destroy_cluster(zc);
        return -1;                                                                                                           
      } 

      inzqs[i] = ext_q;
    }
  }

  for (i = 0; i < num_consumer_queues; i++) {
    pfring_zc_queue *ext_q = NULL;
    pfring_zc_buffer_pool *ext_pool = NULL;

    /*
     * Note: in case of egress devices, we are creating 
     * dummy queues anyway to keep numeration coherent
     */

    rc = pfring_zc_create_queue_pool_pair(zc,
      outdevs[i] == NULL ? queue_len : 1,
      outdevs[i] == NULL ? pool_size : 1,
      &ext_q, &ext_pool);

    if (rc < 0 || ext_q == NULL || ext_q == NULL) {
      trace(TRACE_ERROR, "pfring_zc_create_queue_pool_pair error [%s]\n", strerror(errno));                                             
      pfring_zc_destroy_cluster(zc);
      return -1;                                                                                                           
    } 

    if (outdevs[i] != NULL) { /* Egress queue */
      outzqs[i] = pfring_zc_open_device(zc, outdevs[i], tx_only, 0);

      if (outzqs[i] == NULL) {
        trace(TRACE_ERROR, "pfring_zc_open_device(%s) error [%s]\n", outdevs[i], strerror(errno));
        pfring_zc_destroy_cluster(zc);
        return -1;
      }
    } else {
      outzqs[i] = ext_q;
    }
  }

  if ((inzq_bpf = init_inzq_bpf(&in_bpf_file_list, num_devices))) {
    set_inzq_bpf();
    idle_func = set_inzq_bpf;
    filter_func = packet_filtering_func;
  }
  if ((outzq_bpf = init_outzq_bpf(&out_bpf_file_list, num_consumer_queues))) {
    set_outzq_bpf();
    filter_func = packet_filtering_func;
  }

  wsp = pfring_zc_create_buffer_pool(zc, PREFETCH_BUFFERS);

  if (wsp == NULL) {
    trace(TRACE_ERROR, "pfring_zc_create_buffer_pool error\n");
    pfring_zc_destroy_cluster(zc);
    return -1;
  }

  if (n2disk_producer) {
    char queues_list[256];
    queues_list[0] = '\0';

    for (i = 0; i < n2disk_threads; i++) {
      if (pfring_zc_create_queue(zc, N2DISK_CONSUMER_QUEUE_LEN) == NULL) {
        trace(TRACE_ERROR, "pfring_zc_create_queue error [%s]\n", strerror(errno));
        pfring_zc_destroy_cluster(zc);
        return -1;
      }
      sprintf(&queues_list[strlen(queues_list)], "%ld,", i + num_consumer_queues);
    }
    queues_list[strlen(queues_list)-1] = '\0';

    if (pfring_zc_create_buffer_pool(zc, N2DISK_PREFETCH_BUFFERS + n2disk_threads) == NULL) {
      trace(TRACE_ERROR, "pfring_zc_create_buffer_pool error\n");
      pfring_zc_destroy_cluster(zc);
      return -1;
    }

    trace(TRACE_NORMAL, "Run n2disk10gzc with: -i %d@<queue id> --cluster-ipc-queues %s --cluster-ipc-pool %d --reader-threads <%d core ids>\n", 
      cluster_id, queues_list, num_in_queues + num_consumer_queues + 1, n2disk_threads);
  }

  if (enable_vm_support) {
    vm_sock = strtok(vm_sockets, ",");
    while(vm_sock != NULL) {

      rc = pfring_zc_vm_register(zc, vm_sock);

      if (rc < 0) {
        trace(TRACE_ERROR, "pfring_zc_vm_register(%s) error\n", vm_sock);
        pfring_zc_destroy_cluster(zc);
        return -1;
      }

      vm_sock = strtok(NULL, ",");
    }

    rc = pfring_zc_vm_backend_enable(zc);

    if (rc < 0) {
      trace(TRACE_ERROR, "pfring_zc_vm_backend_enable error\n");
      pfring_zc_destroy_cluster(zc);
      return -1;
    }
  }

  if (pid_file)
    create_pid_file(pid_file);

  signal(SIGINT,  sigproc);
  signal(SIGTERM, sigproc);
  signal(SIGINT,  sigproc);
#ifdef HAVE_ZMQ
  signal(SIGHUP, print_filter);
#endif
  if (inzq_bpf || outzq_bpf)
    signal(SIGUSR1, on_bpf_files_modified);

  if (time_pulse) {
    pulse_timestamp_ns = calloc(CACHE_LINE_LEN/sizeof(u_int64_t), sizeof(u_int64_t));
    pthread_create(&time_thread, NULL, time_pulse_thread, NULL);
    while (!*pulse_timestamp_ns && !do_shutdown); /* wait for ts */
  }

#ifdef HAVE_PF_RING_FT
  if (flow_table) {
    ft = pfring_ft_create_table(PFRING_FT_TABLE_FLAGS_DPI, 0, 0, 0, 0);

    if (ft == NULL) {
      trace(TRACE_ERROR, "pfring_ft_create_table error");
      return -1;
    }

    if (ft_rules_conf != NULL) 
      pfring_ft_load_configuration(ft, ft_rules_conf);

    if (ft_proto_conf != NULL){
      pfring_ft_load_ndpi_protocols(ft, ft_proto_conf);

      notify_fd = inotify_init();
      if (fcntl(notify_fd, F_SETFL, O_NONBLOCK) < 0) // error checking for fcntl
        exit(2);

      notify_wd = inotify_add_watch(notify_fd, ft_proto_conf, IN_MODIFY);

      if (notify_wd == -1)
        trace(TRACE_WARNING, "Could not watch : %s\n", ft_proto_conf);
      else
        trace(TRACE_NORMAL, "Watching protos config at: %s\n", ft_proto_conf);
    }

    /* FT callbacks configuration examples */
    //pfring_ft_set_new_flow_callback(ft, flow_init, NULL);
    //pfring_ft_set_flow_packet_callback(ft, flow_packet_process, NULL);
    //pfring_ft_set_flow_export_callback(ft, flow_free, NULL);
  }
#endif

#ifdef HAVE_ZMQ
  if (zmq_server) {
    src_ip_hash = inplace_alloc(32768);
    dst_ip_hash = inplace_alloc(32768);
    pthread_create(&zmq_thread, NULL, zmq_server_thread, NULL);
  }
#endif

  trace(TRACE_NORMAL, "Starting balancer with %d consumer queues..\n", num_consumer_queues);

  if (num_in_queues > 0) {
    trace(TRACE_NORMAL, "Run your traffic generator as follows:\n");
    for (i = 0; i < num_in_queues; i++)
      trace(TRACE_NORMAL, "\tzsend -i zc:%d@%lu\n", cluster_id, pfring_zc_get_queue_id(inzqs[i]));
  }

  trace(TRACE_NORMAL, "Run your application instances as follows:\n");
  off = 0;
  for (i = 0; i < num_apps; i++) {
    if (num_apps > 1) trace(TRACE_NORMAL, "Application %lu\n", i);
    for (j = 0; j < instances_per_app[i]; j++) {
      if (outdevs[off] == NULL)
        trace(TRACE_NORMAL, "\tpfcount -i zc:%d@%lu\n", cluster_id, pfring_zc_get_queue_id(outzqs[off]));
      else
        trace(TRACE_NORMAL, "\t%s\n", outdevs[off]);
      off++;
    }
  }

  if(wait_time_sec) {
    trace(TRACE_NORMAL, "Sleeping %u sec...", wait_time_sec);
    sleep(wait_time_sec);    
  }

  trace(TRACE_NORMAL, "Running...");

  if (hash_mode == 0 || 
      ((hash_mode == 1 || 
        hash_mode == 4 || 
        hash_mode == 5 || 
        hash_mode == 6 || 
        hash_mode == 7) && 
       num_apps == 1)) { /* balancer */

    switch (hash_mode) {
      case 0: distr_func = rr_distribution_func;
      break;
      case 1: 
        if (strcmp(device, "sysdig") == 0) 
          distr_func = sysdig_distribution_func; 
        else if (time_pulse) 
          distr_func = ip_distribution_func; /* else built-in IP-based */
      break;
      case 4: 
        if (strcmp(device, "sysdig") == 0) 
          distr_func = sysdig_distribution_func; 
        else 
          distr_func = gtp_distribution_func;
      break;
      case 5: 
        if (strcmp(device, "sysdig") == 0) 
          distr_func = sysdig_distribution_func;  
        else 
          distr_func = gre_distribution_func;
      break;
      case 6: 
        distr_func =  direct_distribution_func;
      break;
      case 7: 
        distr_func =  eth_distribution_func;
      break;
    }

    zw = pfring_zc_run_balancer_v2(
      inzqs, 
      outzqs, 
      num_devices, 
      num_consumer_queues,
      wsp,
      round_robin_bursts_policy,
      idle_func,
      filter_func,
      NULL,
      distr_func,
      (void *) ((long) num_consumer_queues),
      !wait_for_packet, 
      bind_worker_core
    );

  } else { /* fanout */
    outzmq = pfring_zc_create_multi_queue(outzqs, num_consumer_queues);

    if (outzmq == NULL) {
      trace(TRACE_ERROR, "pfring_zc_create_multi_queue error [%s]\n", strerror(errno));
      pfring_zc_destroy_cluster(zc);
      return -1;
    }

    switch (hash_mode) {
      case 1: 
        distr_func = fo_multiapp_ip_distribution_func;
      break;
      case 2: 
        if (time_pulse) 
          distr_func = fo_distribution_func; /* else built-in send-to-all */
      break;
      case 3: 
        distr_func = fo_rr_distribution_func;
      break;
      case 4: 
        distr_func = fo_multiapp_gtp_distribution_func;
      break;
      case 5:
        distr_func = fo_multiapp_gre_distribution_func;
      break;
      case 6: 
        distr_func = fo_multiapp_direct_distribution_func;
      break;
    }

    zw = pfring_zc_run_fanout_v2(
      inzqs, 
      outzmq, 
      num_devices,
      wsp,
      round_robin_bursts_policy, 
      idle_func,
      filter_func,
      NULL,
      distr_func,
      (void *) ((long) num_consumer_queues),
      !wait_for_packet, 
      bind_worker_core
    );

  }

  if (zw == NULL) {
    trace(TRACE_ERROR, "pfring_zc_run_balancer error [%s]", strerror(errno));
    pfring_zc_destroy_cluster(zc);
    return -1;
  }

  /* Bind also main thread to the worker core */
  bind2core(bind_worker_core);
  
  if (user != NULL) {
    if (drop_privileges(user) == 0)
      trace(TRACE_NORMAL, "User changed to %s", user);
    else
      trace(TRACE_ERROR, "Unable to drop privileges");
  }

  while (!do_shutdown) {
    sleep(ALARM_SLEEP);
    print_stats();
#ifdef HAVE_PF_RING_FT
    if (flow_table) {
      if (ft_proto_conf != NULL) {
        int i = 0, length;
        char buffer[EVENT_BUF_LEN];
        /* Read buffer*/
        length = read(notify_fd, buffer, EVENT_BUF_LEN);

        /* Process the events which has occurred */
        while (i < length) {
          struct inotify_event *event = (struct inotify_event *)&buffer[i];

          if (event->len) {
            if (event->mask & IN_MODIFY) {
              trace(TRACE_NORMAL, "The protos config at %s was modified\n", event->name);
              //Reload config
              //FIXX this should be moved inline
              pfring_ft_load_ndpi_protocols(ft, ft_proto_conf);
            }
          }
          i += EVENT_SIZE + event->len;
        }
      }
    }
#endif
  }

#ifdef HAVE_ZMQ
  if (zmq_server) {
    zmq_server_breakloop();
    pthread_join(zmq_thread, NULL);
    inplace_free(src_ip_hash);
    inplace_free(dst_ip_hash);
  }
#endif

#ifdef HAVE_PF_RING_FT
  if (flow_table) {
    pfring_ft_destroy_table(ft);
  }
#endif

  if (time_pulse)
    pthread_join(time_thread, NULL);

  pfring_zc_destroy_cluster(zc);

  if (pid_file)
    remove_pid_file(pid_file);

  return 0;
}

