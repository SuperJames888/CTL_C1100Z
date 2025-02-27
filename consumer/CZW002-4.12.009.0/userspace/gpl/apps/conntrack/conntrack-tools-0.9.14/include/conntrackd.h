#ifndef _CONNTRACKD_H_
#define _CONNTRACKD_H_

#include "mcast.h"
#include "local.h"
#include "alarm.h"
#include "filter.h"
#include "channel.h"
#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>
#include <syslog.h>

/* UNIX facilities */
#define FLUSH_MASTER	0	/* flush kernel conntrack table 	*/
#define RESYNC_MASTER	1	/* resync with kernel conntrack table 	*/
#define DUMP_INTERNAL 	16	/* dump internal cache 			*/
#define DUMP_EXTERNAL 	17	/* dump external cache 			*/
#define COMMIT		18	/* commit external cache		*/
#define FLUSH_CACHE	19	/* flush cache				*/
#define KILL		20	/* kill conntrackd			*/
#define STATS		21	/* dump statistics			*/
#define SEND_BULK	22	/* send a bulk				*/
#define REQUEST_DUMP	23	/* request dump 			*/
#define DUMP_INT_XML	24	/* dump internal cache in XML		*/
#define DUMP_EXT_XML	25	/* dump external cache in XML		*/
#define RESET_TIMERS	26	/* reset kernel timers			*/
#define DEBUG_INFO	27	/* unused				*/
#define STATS_NETWORK	28	/* extended network stats		*/
#define STATS_CACHE	29	/* extended cache stats			*/
#define STATS_RUNTIME	30	/* extended runtime stats		*/
#define STATS_LINK	31	/* dedicated link stats			*/
#define STATS_RSQUEUE	32	/* resend queue stats			*/
#define FLUSH_INT_CACHE	33	/* flush internal cache			*/
#define FLUSH_EXT_CACHE	34	/* flush external cache			*/
#define STATS_PROCESS	35	/* child process stats			*/
#define STATS_QUEUE	36	/* queue stats				*/

#define DEFAULT_CONFIGFILE	"/etc/conntrackd/conntrackd.conf"
#define DEFAULT_LOCKFILE	"/var/lock/conntrackd.lock"
#define DEFAULT_LOGFILE		"/var/log/conntrackd.log"
#define DEFAULT_STATS_LOGFILE	"/var/log/conntrackd-stats.log"
#define DEFAULT_SYSLOG_FACILITY	LOG_DAEMON

/* daemon/request modes */
#define NOT_SET         0
#define DAEMON		1
#define REQUEST		2

/* conntrackd modes */
#define CTD_SYNC_MODE		(1UL << 0)
#define CTD_STATS_MODE		(1UL << 1)
#define CTD_SYNC_FTFW		(1UL << 2)
#define CTD_SYNC_ALARM		(1UL << 3)
#define CTD_SYNC_NOTRACK	(1UL << 4)
#define CTD_POLL		(1UL << 5)

/* FILENAME_MAX is 4096 on my system, perhaps too much? */
#ifndef FILENAME_MAXLEN
#define FILENAME_MAXLEN 256
#endif

union inet_address {
	uint32_t ipv4;
	uint32_t ipv6[4];
	uint32_t all[4];
};

#define CONFIG(x) conf.x

struct ct_conf {
	char logfile[FILENAME_MAXLEN];
	int syslog_facility;
	char lockfile[FILENAME_MAXLEN];
	int hashsize;			/* hashtable size */
	int channel_num;
	int channel_default;
	int channel_type_global;
	struct channel_conf channel[MULTICHANNEL_MAX];
	struct local_conf local;	/* unix socket facilities */
	int nice;
	int limit;
	int refresh;
	int cache_timeout;		/* cache entries timeout */
	int commit_timeout;		/* committed entries timeout */
	unsigned int purge_timeout;	/* purge kernel entries timeout */
	unsigned int netlink_buffer_size;
	unsigned int netlink_buffer_size_max_grown;
	int nl_overrun_resync;
	unsigned int flags;
	int family;			/* protocol family */
	unsigned int resend_queue_size; /* FTFW protocol */
	unsigned int window_size;
	int poll_kernel_secs;
	int filter_from_kernelspace;
	int event_iterations_limit;
	struct {
		int error_queue_length;
	} channelc;
	struct {
		int internal_cache_disable;
		int external_cache_disable;
	} sync;
	struct {
		int events_reliable;
	} netlink;
	struct {
		int commit_steps;
	} general;
	struct {
		int type;
		int prio;
	} sched;
	struct {
		char logfile[FILENAME_MAXLEN];
		int syslog_facility;
		size_t buffer_size;
	} stats;
};

#define STATE(x) st.x

struct ct_general_state {
	sigset_t 			block;
	FILE 				*log;
	FILE				*stats_log;
	struct local_server		local;
	struct ct_mode 			*mode;
	struct ct_filter		*us_filter;

	struct nfct_handle		*event;         /* event handler */
	struct nfct_filter		*filter;	/* event filter */
	int				event_iterations_limit;

	struct nfct_handle		*dump;		/* dump handler */
	struct nfct_handle		*resync;	/* resync handler */
	struct nfct_handle		*get;		/* get handler */
	int				get_retval;	/* hackish */
	struct nfct_handle		*flush;		/* flusher */

	struct alarm_block		resync_alarm;
	struct alarm_block		polling_alarm;

	struct fds			*fds;

	/* statistics */
	struct {
		uint64_t 		bytes_orig;
		uint64_t 		bytes_repl;
		uint64_t 		packets_orig;
		uint64_t 		packets_repl;

		time_t			daemon_start_time;

		uint64_t		nl_events_received;
		uint64_t		nl_events_filtered;
		uint32_t		nl_events_unknown_type;
		uint32_t		nl_catch_event_failed;
		uint32_t		nl_overrun;
		uint32_t		nl_dump_unknown_type;
		uint32_t		nl_kernel_table_flush;
		uint32_t		nl_kernel_table_resync;

		uint32_t		child_process_failed;
		uint32_t		child_process_error_segfault;
		uint32_t		child_process_error_term;

		uint32_t		select_failed;
		uint32_t		wait_failed;

		uint32_t		local_read_failed;
		uint32_t		local_unknown_request;

	} stats;
};

#define STATE_SYNC(x) state.sync->x

struct ct_sync_state {
	struct external_handler *external;

	struct multichannel	*channel;
	struct nlif_handle	*interface;
	struct queue *tx_queue;

#define COMMIT_STATE_INACTIVE	0
#define COMMIT_STATE_MASTER	1
#define COMMIT_STATE_RELATED	2

	struct {
		int			state;
		int			clientfd;
		struct nfct_handle	*h;
		struct evfd		*evfd;
		int			current;
		struct {
			int 		ok;
			int		fail;
			struct timeval	start;
		} stats;
	} commit;

	struct alarm_block		reset_cache_alarm;

	struct sync_mode *sync;		/* sync mode */

	/* statistics */
	struct {
		uint64_t	msg_rcv_malformed;
		uint32_t	msg_rcv_bad_version;
		uint32_t	msg_rcv_bad_payload;
		uint32_t	msg_rcv_bad_header;
		uint32_t	msg_rcv_bad_type;
		uint32_t	msg_rcv_truncated;
		uint32_t	msg_rcv_bad_size;
		uint32_t	msg_snd_malformed;
		uint64_t	msg_rcv_lost;
		uint64_t	msg_rcv_before;
	} error;

	uint32_t last_seq_sent;	/* last sequence number sent */
	uint32_t last_seq_recv;	/* last sequence number recv */
};

#define STATE_STATS(x) state.stats->x

struct ct_stats_state {
	struct cache *cache;            /* internal events cache (netlink) */
};

union ct_state {
	struct ct_sync_state *sync;
	struct ct_stats_state *stats;
};

extern struct ct_conf conf;
extern union ct_state state;
extern struct ct_general_state st;

struct ct_mode {
	struct internal_handler *internal;
	int (*init)(void);
	void (*run)(fd_set *readfds);
	int (*local)(int fd, int type, void *data);
	void (*kill)(void);
};

/* conntrackd modes */
extern struct ct_mode sync_mode;
extern struct ct_mode stats_mode;

#define MAX(x, y) x > y ? x : y

/* These live in run.c */
void killer(int foo);
int init(void);
void run(void);

/* from read_config_yy.c */
int
init_config(char *filename);

#endif
