/* dhcpd.c
 *
 * udhcp DHCP client
 *
 * Russ Dill <Russ.Dill@asu.edu> July 2001
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
 
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <errno.h>

#include "dhcpd.h"
#include "dhcpc.h"
#include "options.h"
#include "clientpacket.h"
#include "packet.h"
#include "script.h"
#include "socket.h"
#include "debug.h"
#include "pidfile.h"
#if 1//__MTS__,KuanJung
#include "arpping.h"
#endif

// brcm
#include "cms_msg.h"
#if 1 // __CenturyLink__, Hong-Yu
#include "cms_util.h"
#endif
/*
 * Since dhcpc and dhcpd are actually the same binary, the msgHandle
 * is declared in dhcpd.c and used by dhcpc.
 */

static int state;
static unsigned long requested_ip; /* = 0 */
static unsigned long server_addr;
#if 1 // __CTLK__, SeanLu 20150910, Sending the ARP to default gateway
static unsigned long gateway_addr;
#endif
static unsigned long timeout;
static int packet_num; /* = 0 */
#if 0//__CTLK__, Fritz
static int discover_num=0;
#endif 

// brcm
char session_path[64];
static char status_path[128]="";
static char pid_path[128]="";
char en_vendor_class_id=0;
char vendor_class_id[256]="";
char iaid[10]="";
char duid[256]="";
char en_client_id=0;
char en_125=0;
char oui_125[10]="";
char sn_125[64]="";
char prod_125[64]="";
int ipv6rd_opt = 0;
#if 1//__CTLK__,JhenYang
//vlanautohunt
int en_VAH = 0;
//default service
UBOOL8 default_connection = FALSE;
#endif


#define LISTEN_NONE 0
#define LISTEN_KERNEL 1
#define LISTEN_RAW 2
static int listen_mode = LISTEN_RAW;
// brcm
static int old_mode = LISTEN_RAW;
#define INIT_TIMEOUT 5
#define REQ_TIMEOUT 4

#define DEFAULT_SCRIPT	"/etc/dhcp/dhcp_getdata"

#if 1//__MTS__,JhenYang
#define ARPTIMEOUT 60
#define MAXPROBEFAIL 15
#endif

struct client_config_t client_config = {
	/* Default options. */
	abort_if_no_lease: 0,
	foreground: 0,
	quit_after_lease: 0,
	interface: "eth0",
	pidfile: NULL,
	script: DEFAULT_SCRIPT,
	clientid: NULL,
	hostname: NULL,
	ifindex: 0,
	arp: "\0\0\0\0\0\0",		/* appease gcc-3.0 */
};


// brcm

void sendEventMessage(UBOOL8 assigned, UBOOL8 isExpired, DhcpcStateChangedMsgBody *options)
{
   char buf[sizeof(CmsMsgHeader) + sizeof(DhcpcStateChangedMsgBody)]={0};
   CmsMsgHeader *msg=(CmsMsgHeader *) buf;
   DhcpcStateChangedMsgBody *dhcpcBody = (DhcpcStateChangedMsgBody *) (msg+1);
   CmsRet ret;

   msg->type = CMS_MSG_DHCPC_STATE_CHANGED;
   msg->src = MAKE_SPECIFIC_EID(getpid(), EID_DHCPC);
   msg->dst = EID_SSK;
   msg->flags_event = 1;
   msg->dataLength = sizeof(DhcpcStateChangedMsgBody);

   if (assigned)
   {
      memcpy(dhcpcBody, options, sizeof(DhcpcStateChangedMsgBody));
   }
   dhcpcBody->addressAssigned = assigned;
   dhcpcBody->isExpired = isExpired;

   if ((ret = cmsMsg_send(msgHandle, msg)) != CMSRET_SUCCESS)
   {
      cmsLog_error("could not send out DHCPC_STATUS_CHANGED, ret=%d", ret);
   }
   else
   {
      cmsLog_notice("sent out DHCPC_STATUS_CHANGED (assigned=%d)", assigned);
   }

   return;
}

#if 1//__CTLK__,JhenYang,vlanautohunt
void sendMsgToVAH(CmsMsgType msgType)
{
	CmsMsgHeader *msgHdr;
	CmsRet ret;

	msgHdr = (CmsMsgHeader *) cmsMem_alloc(sizeof(CmsMsgHeader), ALLOC_ZEROIZE);
	if (msgHdr == NULL)
	{
		cmsLog_error("message header allocation failed");
		return;
	}

	msgHdr->src = EID_DHCPC;
	msgHdr->dst = EID_VLANAUTOHUNT;
	msgHdr->type = msgType;
	msgHdr->flags_event = 1;
	msgHdr->dataLength = 0;

	if ((ret = cmsMsg_send(msgHandle, msgHdr)) != CMSRET_SUCCESS)
	{
		cmsLog_error("failed to send event msg 0x%x to smd, ret=%d", msgType, ret);
	}

	cmsMem_free(msgHdr);

	return;
}
#endif

#if 1 // __CTLK__, Richard Huang
void sendMsgToUpdateNTPSrv(const char *ntpSrv)
{
	char ntpBuf[sizeof(CmsMsgHeader) + sizeof(DhcpcNTPChangedMsgBody)] = {0};
	CmsMsgHeader *ntpMsg =(CmsMsgHeader *)ntpBuf;
	DhcpcNTPChangedMsgBody *dhcpcNtpBody = (DhcpcNTPChangedMsgBody *)(ntpMsg+1);
	CmsRet ntpRet;
   
	ntpMsg->type = CMS_MSG_DHCPC_NTP_CHANGED;
	ntpMsg->src = EID_DHCPC;
	ntpMsg->dst = EID_SSK;
	ntpMsg->flags_event = 1;
	ntpMsg->dataLength = sizeof(DhcpcNTPChangedMsgBody);

	strcpy(dhcpcNtpBody->ntpserver, ntpSrv);

	if ((ntpRet = cmsMsg_send(msgHandle, ntpMsg)) != CMSRET_SUCCESS) {
		cmsLog_error("could not send out DHCPC_STATUS_CHANGED, ret=%d", ntpRet);
	} else {
		cmsLog_notice("sent out CMS_MSG_DHCPC_NTP_CHANGED");
	}
}
#endif

void setStatus(int status)
{
   static int wasAssigned=0;

   if (status == 1)
   {
      wasAssigned = 1;
      /*
       * We don't have to send out a DHCPC_STATUS_CHANGED msg here.
       * We did that from run_script.
       */
   }
   else
   {
      /*
       * We went from assigned to un-assigned, send a DHCPC_STATUS_CHANGED
       * msg.
       */
      if (wasAssigned == 1)
      {
         wasAssigned = 0;
         sendEventMessage(FALSE, FALSE, NULL);
      }
   }

   return;
}

void setPid(void) {
    char cmd[128] = "";
    
    sprintf(cmd, "echo %d > %s", getpid(), pid_path);
    system(cmd); 
}

static void print_usage(void)
{
	printf(
"Usage: udhcpcd [OPTIONS]\n\n"
"  -c, --clientid=CLIENTID         Client identifier\n"
"  -H, --hostname=HOSTNAME         Client hostname\n"
"  -f, --foreground                Do not fork after getting lease\n"
"  -i, --interface=INTERFACE       Interface to use (default: eth0)\n"
"  -n, --now                       Exit with failure if lease cannot be\n"
"                                  immediately negotiated.\n"
"  -p, --pidfile=file              Store process ID of daemon in file\n"
"  -q, --quit                      Quit after obtaining lease\n"
"  -r, --request=IP                IP address to request (default: none)\n"
"  -s, --script=file               Run file at dhcp events (default:\n"
"                                  " DEFAULT_SCRIPT ")\n"
"  -v, --version                   Display version\n"
	);
}


/* SIGUSR1 handler (renew) */
static void renew_requested(int sig)
{
	sig = 0;
	LOG(LOG_INFO, "Received SIGUSR1");
#if 1//__MTS__,JhenYang
   if(state == SERVERPROBE)
      state = BOUND;
#endif
	if (state == BOUND || state == RENEWING || state == REBINDING ||
	    state == RELEASED) {
	    	listen_mode = LISTEN_KERNEL;
		server_addr = 0;
#if 1 // __CTLK__, SeanLu 20150910, Sending the ARP to default gateway
		gateway_addr = 0;
#endif
		packet_num = 0;
		state = RENEW_REQUESTED;
	}

	if (state == RELEASED) {
		listen_mode = LISTEN_RAW;
		state = INIT_SELECTING;
	}

	/* Kill any timeouts because the user wants this to hurry along */
	timeout = 0;
}


/* SIGUSR2 handler (release) */
static void release_requested(int sig)
{
	sig = 0;
	LOG(LOG_INFO, "Received SIGUSR2");
#if 1//__MTS__,JhenYang
   if(state == SERVERPROBE)
      state = BOUND;
#endif
	/* send release packet */
	if (state == BOUND || state == RENEWING || state == REBINDING) {
		send_release(server_addr, requested_ip); /* unicast */
		run_script(NULL, "deconfig");
#if 1//__CTLK__, Fritz
		cmsLed_setInternetLed("off");
#endif
	}

	listen_mode = 0;
	state = RELEASED;
	timeout = 0xffffffff;
}


/* Exit and cleanup */
static void exit_client(int retval)
{
	pidfile_delete(client_config.pidfile);
	CLOSE_LOG();
	exit(retval);
}


/* SIGTERM handler */
static void terminate(int sig)
{
	sig = 0;
	LOG(LOG_INFO, "Received SIGTERM");
	exit_client(0);
}


static void background(void)
{
	int pid_fd;
	if (client_config.quit_after_lease) {
		exit_client(0);
	} else if (!client_config.foreground) {
		pid_fd = pidfile_acquire(client_config.pidfile); /* hold lock during fork. */
		switch(fork()) {
		case -1:
			perror("fork");
			exit_client(1);
			/*NOTREACHED*/
		case 0:
			// brcm
			setPid();
			break; /* child continues */
		default:
			exit(0); /* parent exits */
			/*NOTREACHED*/
		}
		close(0);
		close(1);
		close(2);
		setsid();
		client_config.foreground = 1; /* Do not fork again. */
		pidfile_write_release(pid_fd);
	}
}



#ifdef COMBINED_BINARY
int udhcpc(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
	char *temp, *message;
	unsigned long t1 = 0, t2 = 0, xid = 0;
	unsigned long start = 0, lease;
	fd_set rfds;
	int fd, retval;
	struct timeval tv;
	int c, len;
	struct ifreq ifr;
	struct dhcpMessage packet;
	struct in_addr temp_addr;
	int pid_fd;
#if 1//__MTS__,JhenYang
   int tmpTs;
   int arpRet=0;
   int probeFail=0;
#endif

	static struct option options[] = {
		{"clientid",	required_argument,	0, 'c'},
		{"foreground",	no_argument,		0, 'f'},
		{"hostname",	required_argument,	0, 'H'},
		{"help",	no_argument,		0, 'h'},
		{"interface",	required_argument,	0, 'i'},
		{"now", 	no_argument,		0, 'n'},
		{"pidfile",	required_argument,	0, 'p'},
		{"quit",	no_argument,		0, 'q'},
		{"request",	required_argument,	0, 'r'},
		{"script",	required_argument,	0, 's'},
		{"version",	no_argument,		0, 'v'},
		{"6rd",	no_argument,		0, '6'},
		{0, 0, 0, 0}
	};

	/* get options */
	while (1) {
		int option_index = 0;
// brcm
#if 1//__CTLK__,JhenYang, refer to Q1000Z, wan host and domain name; "V" for vlanautohunt; "N" defulat connection
		c = getopt_long(argc, argv, "c:fH:hi:np:qr:s:d:v:6I:D:O:S:P:M:VN", options, &option_index);
#else
		c = getopt_long(argc, argv, "c:fH:hi:np:qr:s:d:v:6I:D:O:S:P:", options, &option_index);
#endif
		if (c == -1) break;
		
		switch (c) {
		case 'c':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.clientid) free(client_config.clientid);
			client_config.clientid = malloc(len + 2);
			client_config.clientid[OPT_CODE] = DHCP_CLIENT_ID;
			client_config.clientid[OPT_LEN] = len;
			strncpy(client_config.clientid + 2, optarg, len);
			break;
		case 'f':
			client_config.foreground = 1;
			break;
		case 'H':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.hostname) free(client_config.hostname);
			client_config.hostname = malloc(len + 2);
			client_config.hostname[OPT_CODE] = DHCP_HOST_NAME;
			client_config.hostname[OPT_LEN] = len;
			strncpy(client_config.hostname + 2, optarg, len);
			break;
#if 1//__CTLK__,JhenYang, refer to Q1000Z, wan host and domain name
		case 'M':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.domainName) free(client_config.domainName);
			client_config.domainName = malloc(len + 2);
			client_config.domainName[OPT_CODE] = DHCP_DOMAIN_NAME;
			client_config.domainName[OPT_LEN] = len;
			strncpy(client_config.domainName + 2, optarg, len);
			break;
#endif
		case 'h':
			print_usage();
			return 0;
		case 'i':
			client_config.interface =  optarg;
// brcm
			strcpy(session_path, optarg);
			break;
		case 'n':
			client_config.abort_if_no_lease = 1;
			break;
		case 'p':
			client_config.pidfile = optarg;
			break;
		case 'q':
			client_config.quit_after_lease = 1;
			break;
		case 'r':
			requested_ip = inet_addr(optarg);
			break;
// brcm
		case 'd':
			strcpy(vendor_class_id, optarg);
			en_vendor_class_id=1;
			break;
		case '6':
			ipv6rd_opt = 1;
			break;
		case 's':
			client_config.script = optarg;
			break;
		case 'v':
			printf("udhcpcd, version %s\n\n", VERSION);
			break;
		case 'I':
			strcpy(iaid, optarg);
            en_client_id = 1;
			break;
		case 'D':
			strcpy(duid, optarg);
            en_client_id = 1;
			break;
		case 'O':
			strcpy(oui_125, optarg);
			en_125 = 1;
			break;
		case 'S':
			strcpy(sn_125, optarg);
			en_125 = 1;
			break;
		case 'P':
			strcpy(prod_125, optarg);
			en_125 = 1;
			break;
#if 1//__CTLK__,JhenYang,vlanautohunt
		case 'V':
			en_VAH = 1;
			break;
		case 'N':
			default_connection = TRUE;
			break;
#endif
		}
	}

	// brcm
        if (strlen(session_path) > 0) {
	    sprintf(status_path, "%s/%s/%s", _PATH_WAN_DIR, session_path, _PATH_MSG);
	    sprintf(pid_path, "%s/%s/%s", _PATH_WAN_DIR, session_path, _PATH_PID);
	}

	OPEN_LOG("udhcpc");
#ifdef VERBOSE
   cmsLog_setLevel(LOG_LEVEL_DEBUG);
#else
   cmsLog_setLevel(DEFAULT_LOG_LEVEL);
#endif

	LOG(LOG_INFO, "udhcp client (v%s) started", VERSION);

   cmsMsg_init(EID_DHCPC, &msgHandle);

	pid_fd = pidfile_acquire(client_config.pidfile);
	pidfile_write_release(pid_fd);

	if ((fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) >= 0) {
		strcpy(ifr.ifr_name, client_config.interface);
		if (ioctl(fd, SIOCGIFINDEX, &ifr) == 0) {
			DEBUG(LOG_INFO, "adapter index %d", ifr.ifr_ifindex);
			client_config.ifindex = ifr.ifr_ifindex;
		} else {
			LOG(LOG_ERR, "SIOCGIFINDEX failed! %s", strerror(errno));
			exit_client(1);
		}
		if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
			memcpy(client_config.arp, ifr.ifr_hwaddr.sa_data, 6);
			DEBUG(LOG_INFO, "adapter hardware address %02x:%02x:%02x:%02x:%02x:%02x",
				client_config.arp[0], client_config.arp[1], client_config.arp[2], 
				client_config.arp[3], client_config.arp[4], client_config.arp[5]);
		} else {
			LOG(LOG_ERR, "SIOCGIFHWADDR failed! %s", strerror(errno));
			exit_client(1);
		}
	} else {
		LOG(LOG_ERR, "socket failed! %s", strerror(errno));
		exit_client(1);
	}
	close(fd);
	fd = -1;

	/* setup signal handlers */
	signal(SIGUSR1, renew_requested);
	signal(SIGUSR2, release_requested);
	signal(SIGTERM, terminate);
	
	state = INIT_SELECTING;
	// brcm
	// run_script(NULL, "deconfig");

	// brcm
	setStatus(0);
	for (;;) {

		// brcm
		if ((old_mode != listen_mode) || (fd == -1)) {
		    old_mode = listen_mode;
		
        /*
        * After dhcpc runs as daemon(backgroud)mode, fd 0-2 be closed.  
        *  The sock fd may be 0.
        */
		    if (fd >= 0) {
			    close(fd);
			    fd = -1;
		    }
		
		    if (listen_mode == LISTEN_RAW) {
			    if ((fd = raw_socket(client_config.ifindex)) < 0) {
				    LOG(LOG_ERR, "couldn't create raw socket -- au revoir");
				    exit_client(0);
			    }
		    }
		    else if (listen_mode == LISTEN_KERNEL) {
			    if ((fd = listen_socket(INADDR_ANY, CLIENT_PORT, client_config.interface)) < 0) {
				    LOG(LOG_ERR, "couldn't create server socket -- au revoir");
				    exit_client(0);
			    }			
		    } else 
			fd = -1;
		}

		tv.tv_sec = timeout - time(0);
		tv.tv_usec = 0;
		FD_ZERO(&rfds);

#if 1//__MTS__,JhenYang
      if( state==BOUND || state==SERVERPROBE)
      {
      	if ( arpRet)
         tmpTs=ARPTIMEOUT - 2;
		else
		 tmpTs=ARPTIMEOUT;
         state=SERVERPROBE;

         if(tv.tv_sec<tmpTs)
         {
            tmpTs = tv.tv_sec;
            state = BOUND;
         }
         tv.tv_sec = tmpTs;
      }

	  if(state!=SERVERPROBE)
      {
         probeFail=0;
      }
      DEBUG(LOG_INFO, "timeout:%d", timeout);
      DEBUG(LOG_INFO, "state:%d", state);
	  DEBUG(LOG_INFO, "tv_sec:%d", tv.tv_sec);
#endif
#if 1//__CTLK__,JhenYang,merge from 406
		if (listen_mode && (fd != -1)) FD_SET(fd, &rfds);
#else
		if (listen_mode) FD_SET(fd, &rfds);
#endif
		if (tv.tv_sec > 0) {
			retval = select(fd + 1, &rfds, NULL, NULL, &tv);
		} else retval = 0; /* If we already timed out, fall through */

		if (retval == 0) {
			/* timeout dropped to zero */
			switch (state) {
			case INIT_SELECTING:
				// brcm
				setStatus(0);
				if (packet_num < 3) {
					if (packet_num == 0)
						xid = random_xid();

					/* send discover packet */
					send_discover(xid, requested_ip); /* broadcast */
					
					timeout = time(0) + ((packet_num == 2) ? REQ_TIMEOUT : 2);
					packet_num++;
				} else {
					if (client_config.abort_if_no_lease) {
						LOG(LOG_INFO,
						    "No lease, failing.");
						exit_client(1);
				  	}
					/* wait to try again */
					packet_num = 0;
					timeout = time(0) + INIT_TIMEOUT;
#if 1//__CTLK__, Fritz
                    cmsLed_setInternetLed("red");
#else
					if (discover_num <= 3) {
						discover_num++;
					} else {
						discover_num = 0;
						cmsLed_setInternetLed("red");
					}
#endif
				}
				break;
			case RENEW_REQUESTED:
			case REQUESTING:
				if (packet_num < 3) {
					/* send request packet */
					if (state == RENEW_REQUESTED)
						send_renew(xid, server_addr, requested_ip); /* unicast */
					else send_selecting(xid, server_addr, requested_ip); /* broadcast */
					
					timeout = time(0) + ((packet_num == 2) ? REQ_TIMEOUT : 2);
					packet_num++;
				} else {
					/* timed out, go back to init state */
					state = INIT_SELECTING;
					timeout = time(0);
					packet_num = 0;
					listen_mode = LISTEN_RAW;
#if 1 // __CenturyLink__, Hong-Yu
					if(default_connection) {
						cmsLed_setInternetLed("red");
					}
					LOG(LOG_ERR, "Cannot get DHCP ACK response!!");
#endif
				}
				break;
#if 1//__MTS__,JhenYang
         case SERVERPROBE:
            arpRet=0;
            arpRet=arpping(server_addr, requested_ip, client_config.interface);
	
            DEBUG(LOG_INFO, "arpRet:%x",arpRet);
            if(arpRet)//probe fail
            {
#if 1 // __CTLK__, SeanLu 20150908, Send ARP to default gateway if arp server fail
               arpRet=arpping(gateway_addr, requested_ip, client_config.interface);
               if(!arpRet)
               {
                  probeFail = 0; //reset probe times
                  break;
               }
#endif
            	probeFail++;
#if 1 // __CTLK__, SeanLu 20150908, Support send ICMP messages to check server alive
               printf("BOTH ARP Fail, show counter = %d", probeFail);
#endif
               if(probeFail<MAXPROBEFAIL)
                  break;
               probeFail = 0;
			   run_script(NULL, "deconfig");
               listen_mode = LISTEN_RAW;
               state = INIT_SELECTING;
               requested_ip = 0;
               timeout = time(0);
               packet_num = 0;
            }
#if 1 // __CTLK__, SeanLu 20150908, Support send ICMP messages to check server alive
            else
            {
               probeFail = 0; //reset probe times
            }
#endif
            break;
#endif
			case BOUND:
				/* Lease is starting to run out, time to enter renewing state */
				state = RENEWING;
				listen_mode = LISTEN_KERNEL;
#if 1//__CTLK__, Fritz
				packet_num = 0;
#endif
				DEBUG(LOG_INFO, "Entering renew state");
				/* fall right through */
			case RENEWING:
				/* Either set a new T1, or enter REBINDING state */
				if ((t2 - t1) <= (lease / 14400 + 1)) {
					/* timed out, enter rebinding state */
					state = REBINDING;
					timeout = time(0) + (t2 - t1);
					DEBUG(LOG_INFO, "Entering rebinding state");
				} else {
					/* send a request packet */
#if 1//__CTLK__, Fritz
					if (packet_num < 3) {
						send_renew(xid, server_addr, requested_ip); /* unicast */
						packet_num++;
					} else {
						if(default_connection) {
							cmsLed_setInternetLed("red");
							LOG(LOG_ERR, "Turn LED to Red");
						}
						LOG(LOG_ERR, "Cannot get DHCP ACK response!!");
					}
#else
					send_renew(xid, server_addr, requested_ip); /* unicast */
#endif
					t1 = (t2 - t1) / 2 + t1;
					timeout = t1 + start;
				}
				break;
			case REBINDING:
				/* Either set a new T2, or enter INIT state */
				if ((lease - t2) <= (lease / 14400 + 1)) {
					/* timed out, enter init state */
					state = INIT_SELECTING;
					LOG(LOG_INFO, "Lease lost, entering init state");
					run_script(NULL, "deconfig");
					timeout = time(0);
					packet_num = 0;
					listen_mode = LISTEN_RAW;
				} else {
					/* send a request packet */
#if 1//__CTLK__, Fritz
					if (packet_num < 3) {
						send_renew(xid, 0, requested_ip); /* broadcast */
						packet_num++;
					} else {
						if(default_connection) {
							cmsLed_setInternetLed("red");
							LOG(LOG_ERR, "Turn LED to Red");
						}
						LOG(LOG_ERR, "Cannot get DHCP ACK response!!");
					}
#else
					send_renew(xid, 0, requested_ip); /* broadcast */
#endif
					t2 = (lease - t2) / 2 + t2;
					timeout = t2 + start;
				}
				break;
			case RELEASED:
				/* yah, I know, *you* say it would never happen */
				timeout = 0xffffffff;
				break;
			}
		} else if (retval > 0 && listen_mode != LISTEN_NONE && FD_ISSET(fd, &rfds)) {
			/* a packet is ready, read it */

			if (listen_mode == LISTEN_KERNEL) {
				if (get_packet(&packet, fd) < 0) continue;
			} else {
				if (get_raw_packet(&packet, fd) < 0) continue;
			} 

			if (packet.xid != xid) {
				DEBUG(LOG_INFO, "Ignoring XID %lx (our xid is %lx)",
					(unsigned long) packet.xid, xid);
				continue;
			}
			
			if ((message = (char *)get_option(&packet, DHCP_MESSAGE_TYPE)) == NULL) {
				DEBUG(LOG_ERR, "couldnt get option from packet -- ignoring");
				continue;
			}
			
			switch (state) {
			case INIT_SELECTING:
				/* Must be a DHCPOFFER to one of our xid's */
				if (*message == DHCPOFFER) {
#if 1//__CTLK__,JhenYang,vlanautohunt
					if(en_VAH) {
						sendMsgToVAH(CMS_MSG_VLANAUTOHUNT_HUNTED);
						exit_client(0);
					}
					syslog(LOG_CRIT,"DHCP receive offer packet\n");
#endif
					if ((temp = (char *)get_option(&packet, DHCP_SERVER_ID))) {
						memcpy(&server_addr, temp, 4);
						xid = packet.xid;
						requested_ip = packet.yiaddr;
						
						/* enter requesting state */
						state = REQUESTING;
						timeout = time(0);
						packet_num = 0;
#if 0//__CTLK__, Fritz
						discover_num = 0;
#endif
					} else {
						DEBUG(LOG_ERR, "No server ID in message");
					}
#if 1 // __CTLK__, SeanLu 20150910, Sending the ARP to default gateway
					if ((temp = (char *)get_option(&packet, DHCP_ROUTER))) {
						memcpy(&gateway_addr, temp, 4);
					} else {
						DEBUG(LOG_ERR, "No router address in message");
					}
#endif
				}
				break;
			case RENEW_REQUESTED:
			case REQUESTING:
			case RENEWING:
			case REBINDING:
				if (*message == DHCPACK) {
#if 1//__CTLK__,JhenYang
					syslog(LOG_CRIT,"DHCP receive ACK option\n");
#endif
#if 1 // __CTLK__, Richard Huang
					if ((temp = (char *)get_option(&packet, DHCP_NTP_SERVER)) != NULL)
					{
						char ipStr[16] = {0};
						unsigned char ipAddr[4];

						memcpy(ipAddr, temp, 4);
						snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ipAddr[0], ipAddr[1], ipAddr[2], ipAddr[3]);

						sendMsgToUpdateNTPSrv(ipStr);
					}
#endif
					if (!(temp = (char *)get_option(&packet, DHCP_LEASE_TIME))) {
						LOG(LOG_ERR, "No lease time with ACK, using 1 hour lease");
						lease = 60*60;
					} else {
						memcpy(&lease, temp, 4);
						lease = ntohl(lease);
					}
						
					/* enter bound state */
					t1 = lease / 2;
					
					/* little fixed point for n * .875 */
					t2 = (lease * 0x7) >> 3;
					temp_addr.s_addr = packet.yiaddr;
					LOG(LOG_INFO, "Lease of %s obtained, lease time %ld", 
						inet_ntoa(temp_addr), lease);
					start = time(0);
					timeout = t1 + start;
					requested_ip = packet.yiaddr;
					run_script(&packet,
						   ((state == RENEWING || state == REBINDING) ? "renew" : "bound"));

#if 1//__CTLK__, Fritz
					if (state == RENEWING || state == REBINDING) {
						if (packet_num >= 3) {
							LOG(LOG_ERR, "RENEWING REBINDING Turn LED to Green");
							cmsLed_setInternetLed("green");
						}
					}
						
#endif
					state = BOUND;
					listen_mode = LISTEN_NONE;
					
					// brcm
                    close(fd);
                    fd = -1;
					setStatus(1);
					background();
					
				} else if (*message == DHCPNAK) {
					/* return to init state */
#if 1//__CTLK__,JhenYang
					syslog(LOG_CRIT,"DHCP receive NAK option\n");
#endif
					LOG(LOG_INFO, "Received DHCP NAK");
					if (state != REQUESTING)
						run_script(NULL, "deconfig");
					state = INIT_SELECTING;
					timeout = time(0);
					requested_ip = 0;
					packet_num = 0;
					listen_mode = LISTEN_RAW;

					// brcm
					setStatus(0);
#if 1//__CTLK__, Fritz
					cmsLed_setInternetLed("red");
#endif
				}
				break;
			case BOUND:
			case RELEASED:
				/* ignore all packets */
				break;
			}					
		} else if (retval == -1 && errno == EINTR) {
			/* a signal was caught */
			
		} else {
			/* An error occured */
			DEBUG(LOG_ERR, "Error on select");
		}
		
	}
	return 0;
}

