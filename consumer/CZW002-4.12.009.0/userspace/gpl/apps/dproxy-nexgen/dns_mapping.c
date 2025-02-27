#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <syslog.h>
#include <linux/if.h>
#include <dirent.h>
#include <fcntl.h>
#include "errno.h"
#include <sys/stat.h>

#include "dproxy.h"
#include "dns_decode.h"
#include "conf.h"
#include "dns_list.h"
#include "dns_construct.h"
#include "dns_io.h"
#include "dns_dyn_cache.h"
#include "prctl.h"
#include "cms.h"
#include "cms_util.h"

/* Used for the interface grouping where the incoming lan ip will be  used to check against 
 * the lanSubnet/lanMask for the correct dns.  Currently, only the dns1 will be used 
 */
typedef struct _DNSInfoEntry
{
   /* LAN IPv4 subnet */
   struct in_addr lanSubnet;
   struct in_addr lanMask;
   /* LAN IPv6 subnet */
   char lanIpv6Subnet[2][INET6_ADDRSTRLEN];
   /* IPv6 address */
   char *ipv4_dns1;
   char *ipv4_dns2;
   char *ipv6_dns1;
   char *ipv6_dns2;
#if 1//__CTLK__, Fritz
   char *orig_ipv4_dns1;
   char *orig_ipv6_dns1;
   UBOOL8 isSwapped;
   unsigned int curr_proto; //0:default, 1:ipv4, 2:ipv6
#endif
   char *processNameList;
   struct _DNSInfoEntry *next;
} DNSInfoEntry;

static DNSInfoEntry *head = NULL;



static UBOOL8 IsValidEphemeralPort(int srcPort)
{
   static int portBegin = 0;
   static int portEnd = 0;
   UBOOL8 found = FALSE;
   
   /* For IPv4, ephemeral port range is in proc/sys/net/ipv4/ip_local_port_range: should be 32768 to 61000
   * and this is also shared by IPv6.
   */

   if (!portBegin)
   {
      char filename[BUFLEN_128];
      FILE *fp;

      cmsUtl_strncpy(filename, "proc/sys/net/ipv4/ip_local_port_range", sizeof(filename)-1);
      
      if ((fp = fopen(filename, "r")) == NULL)
      {
        cmsLog_error("could not open %s", filename);
        return found;
      }

      fscanf(fp, "%d   %d", &portBegin, &portEnd);
      fclose(fp);
      debug("port range [%d-%d]", portBegin, portEnd);
   }
   
   if (srcPort < portBegin || srcPort > portEnd)
   {
      cmsLog_error("Invalid src port %d. Should be in range[%d-%d]", srcPort, portBegin, portEnd);
   }
   else
   {
      found = TRUE;
   }

   return found;

}

static UBOOL8 isProcessMatchInode(const char *processName, unsigned long long inode)
{
   int pid = 0;
   UBOOL8 found = FALSE;
   struct dirent  *dp;
   struct stat statbuf;
   DIR *dir;   
   char fileName[BUFLEN_128]={0};
   char processFdDir[BUFLEN_128]={0};

   debug("%s", processName);
    
   if ((pid = prctl_getPidByName(processName)) == 0)
   {
      debug("Process %s is not in the system", processName);
      return found;
   }

   snprintf(processFdDir, sizeof(processFdDir), "/proc/%d/fd", pid);
   
   dir = opendir(processFdDir);

   /* Need to go over socket with a symbolic link file descriptor
   * to find the related source port
   */
   while (!found && (dp = readdir(dir)) != NULL) 
   {
      /* Get entry's information. */
      snprintf(fileName, sizeof(processFdDir),  "%s/%s", processFdDir, dp->d_name);

      /* use stat to get file stat buffer for later processing */
      if (stat(fileName, &statbuf) == -1)
      {
          perror("opendir");
          continue;
      }

      /* Only interested in symbolly link and socket */           
      if (dp->d_type == DT_LNK && S_ISSOCK(statbuf.st_mode))
      {
         if ((unsigned long long)statbuf.st_ino == inode)
         {
            debug("inode match");
            found = TRUE;
         }
      }
   }   

   closedir(dir);       

   debug("Found=%d", found);
   
   return found;
   
}


/* To Find the inode from udp src port in IPV4:
*
* cat /proc/net/udp, the following will be displayed:
* sl    local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt      uid  timeout   inode ref pointer drops
* 15: 7F000001:E492 7F000001:0035 01 00000000:00000000 00:00000000 00000000     0        0 18143 2 8fa4e5b0 0
*
* Need to find the match with the local_address which is ip:7F00001 (127.0.0.1) and srcPort A890 here and get inode 18143
* from this line.
*/


/* To Find the inode from udp src port in IPV6:
*
* cat /proc/net/udp6, the following will be displayed:
*   sl  local_address                                                 remote_address                                             st  tx_queue  rx_queue   tr  tm->when retrnsmt   uid  timeout inode    ref pointer    drops
* 16: 00000000000000000000000000000001:A310 00000000000000000000000000000001:0035 01 00000000:00000000 00:00000000  00000000 0        0       101264 2   8cf3c050 0
*
* Need to find the match with the local_address which is ip:1 (::1) and srcPort 0035 here and get inode 101264
* from this line.
*/
static unsigned long long getInodeFromSrcPort(int srcPort, UBOOL8 isIpv4)
{
   char filename[BUFLEN_128];
   UBOOL8 found = FALSE;
   FILE *fp = NULL;
   unsigned long long inode = 0;
   char line[BUFLEN_256];
   char scanfFormat[BUFLEN_128];
   SINT32 loopbackAddr = 0;
   
   if (isIpv4)
   {
      snprintf(filename, sizeof(filename), "/proc/net/udp");
      cmsUtl_strncpy(scanfFormat, "%d: %08X:%04X %08X:%04X %02X %08X:%08X %02X:%08X %08X %05X %08X %llu", sizeof(scanfFormat)-1);
      loopbackAddr = INADDR_LOOPBACK;
   }
   else
   {
      snprintf(filename, sizeof(filename), "/proc/net/udp6");
      cmsUtl_strncpy(scanfFormat, "%d: %32d:%04X %32d:%04X %02X %08X:%08X %02X:%08X %08X %05X %08X %llu", sizeof(scanfFormat)-1);
      loopbackAddr = 1;       /* cannot seems to find any defines ? */
   }

  debug("open file %s to find src port %X, loopback=%x", filename, srcPort, loopbackAddr);

   
   if ((fp = fopen(filename, "r")) == NULL)
   {
      cmsLog_error("could not open %s", filename);
      return found;
   }

   while (!found && (fgets(line,sizeof(line), fp) != NULL))
   {
      int sl, udp_ip, udp_srcport, rem_addr1, rem_addr2, st, txq, rxq, tr, tm, retrn, uid, timeout;

      sscanf(line, scanfFormat, &sl, &udp_ip, &udp_srcport, &rem_addr1, &rem_addr2, &st, &txq, &rxq, &tr, &tm, &retrn, &uid, &timeout, &inode);

      if (udp_ip == loopbackAddr && udp_srcport == srcPort)
      {
         found = TRUE;
         debug("FOUND in line=%s", line);         
      }
   }

   fclose(fp);
   
   debug("Found =%d, inode=%llu", found, inode);

   return inode;
   
}                




static UBOOL8 isSourcePortMatchProcess(int srcPort, char *processNameList, UBOOL8 isIpv4)
{
   UBOOL8 found = FALSE;
   char tmpProcessNameList[BUFLEN_256]={0};
   char processName[BUFLEN_128]={0};
   unsigned long long inode = 0;
   char *curPtr = NULL;
   char *nullPtr = NULL;
   
   debug("Enter. srcPort=%0X, processNameList=%s", srcPort, processNameList);
   
   if (processNameList == NULL || !cmsUtl_strcmp(processNameList, ""))
   {
      debug("No need to check.");
      return found;      
   }
   
   if (!IsValidEphemeralPort(srcPort))
   {
      cmsLog_error("Invalid source port %d", srcPort);
      return found;
   }

   /* Try to find the inode from udp source port */
   if ((inode = getInodeFromSrcPort(srcPort, isIpv4)) == 0)
   {
      cmsLog_error("Cannot find the indoe for this srcPort=%d", srcPort);
      return found;      
   }   

   cmsUtl_strncpy(tmpProcessNameList,  processNameList, sizeof(tmpProcessNameList)-1);

   if (strchr(tmpProcessNameList, ',') == NULL) 
   {
      /* for only one process name in the process name list */
      if (isProcessMatchInode(tmpProcessNameList, inode))
      {
         found = TRUE;
      }
   }
   else
   {
      /* If more than one process is the process name list, need try them all */
      curPtr = tmpProcessNameList;
      
      while (!found && (nullPtr = strchr(curPtr, ',')) != NULL)
      {
         *nullPtr = '\0';
         cmsUtl_strncpy(processName, curPtr, sizeof(processName)-1);
         if (isProcessMatchInode(processName, inode))
         {
            found = TRUE;
         }
         curPtr = nullPtr + 1;
      }
   }
   
   debug("Found=%d", found);
   
   return found;
   
}

#if 1//__CTLK__, Fritz, modify dns swap mechanism
static UBOOL8 findDnsServerAddress(DNSInfoEntry *curr, int queryType, 
                                   char *dns1, int *proto, UBOOL8 checkProtoSwap)
#else
static UBOOL8 findDnsServerAddress(DNSInfoEntry *curr, int queryType, 
                                   char *dns1, int *proto)
#endif
{
   UBOOL8 found = FALSE;
#if 1 //__MSTC__, Loujia 0819 Dnsproxy pref
	   UBOOL8 isuseipv4=FALSE;
	   if(dnsserverPref==0) //original condition
		   isuseipv4=(queryType != AAA);
	   else if (dnsserverPref==2&&(curr->ipv6_dns1!=NULL || curr->ipv6_dns2!=NULL ))
		   isuseipv4=FALSE;
	   else
		   isuseipv4=TRUE;
#endif
#if 1//__CTLK__, Fritz, modify dns swap mechanism
	  if (curr->curr_proto!=0) {
	  	if (isuseipv4) {
	  		if (curr->curr_proto==2)
	  			isuseipv4=FALSE;
	  	} else {
	  		if (curr->curr_proto==1)
	  			isuseipv4=TRUE;
	  	}
	  }

	  if (checkProtoSwap) {
	  	if (isuseipv4) {
	  		cmsLog_debug("curr->ipv4_dns1=%s, curr->orig_ipv4_dns1=%s", curr->ipv4_dns1, curr->orig_ipv4_dns1);
	  		if (curr->isSwapped==TRUE && !strcmp(curr->orig_ipv4_dns1, curr->ipv4_dns1)) {
	  			if (curr->ipv6_dns1!=NULL || curr->ipv6_dns2!=NULL) {
	  				cmsLog_debug("Swap IPv4 to IPv6 Proto");
	  				curr->isSwapped=FALSE;
	  				curr->curr_proto=2;
					isuseipv4=FALSE;
	  			}
	  				
	  		}
	  	} else {
	  		cmsLog_debug("curr->ipv6_dns1=%s, curr->orig_ipv6_dns1=%s", curr->ipv6_dns1, curr->orig_ipv6_dns1);
	  		if (curr->isSwapped==TRUE && !strcmp(curr->orig_ipv6_dns1, curr->ipv6_dns1)) {
	  			if (curr->ipv4_dns1!=NULL || curr->ipv4_dns2!=NULL) {
	  				cmsLog_debug("Swap IPv6 to IPv4 Proto");
	  				curr->isSwapped=FALSE;
	  				curr->curr_proto=1;
	  				isuseipv4=TRUE;	  				
	  			}

	  		}
	  	}
	  }
#endif

#if 1 //__MSTC__, Loujia 0819 Dnsproxy pref
	  if (!isuseipv4)  
#else
	  if (queryType == AAA)
#endif   	
   {
      /*
       * AAAA query: If IPv6 DNS server exists, return the IPv6 DNS
       * server address. Otherwise, return the IPv4 DNS server 
       * address if available.
       */
#if 1 //__CTLK__, JhihPing, bugfix CDRouter-ipv6_dns_45,46
      if ( !errorCodeFailover && curr->ipv6_dns1 ) 
#else
      if (curr->ipv6_dns1)
#endif
      {
         strcpy(dns1, curr->ipv6_dns1);
         *proto = AF_INET6;
         found = TRUE;
      }
#if 1 //__CTLK__, JhihPing, bugfix CDRouter-ipv6_dns_45,46
      else if ( errorCodeFailover && curr->ipv6_dns2 && (strcmp(curr->ipv6_dns2, "\0") != 0) )
      {
         strcpy(dns1, curr->ipv6_dns2);
		 *proto = AF_INET6;
         found = TRUE;
      }
#endif
      else if (curr->ipv4_dns1)
      {
         strcpy(dns1, curr->ipv4_dns1);
		 *proto = AF_INET;
         found = TRUE;
      }
   }
   else
   {
      /*
       * A query: If IPv4 DNS server exists, return the IPv4 DNS 
       * server address. Otherwise, return the IPv6 DNS server 
       * address if available.
       */
#if 1 //__CTLK__, JhihPing, bugfix CDRouter-ipv6_dns_45,46
      if ( !errorCodeFailover && curr->ipv4_dns1 ) 
#else
      if (curr->ipv4_dns1)
#endif
      {
         strcpy(dns1, curr->ipv4_dns1);
		 *proto = AF_INET;
         found = TRUE;
      }
#if 1 //__CTLK__, JhihPing, bugfix CDRouter-ipv6_dns_45,46
      else if ( errorCodeFailover && curr->ipv4_dns2 && (strcmp(curr->ipv4_dns2, "\0") != 0) )
      {
         strcpy(dns1, curr->ipv4_dns2);
		 *proto = AF_INET;
         found = TRUE;
      }
#endif
      else if (curr->ipv6_dns1)
      {
         strcpy(dns1, curr->ipv6_dns1);
		 *proto = AF_INET6;
         found = TRUE;
      }
   }
#if 1 //__CTLK__, JhihPing, bugfix CDRouter-ipv6_dns_45,46
   /* No matter which DNS found, errorCodeFailover always sets to 0 */
   errorCodeFailover = 0;
#endif

   return found;
}


void updateIpv4Ipv6Ptr( char *strList, char **curPtr, char **nullPtr )
{
   if (strList == NULL)
   {
      *curPtr = NULL;
      *nullPtr = NULL;
      return;
   }

   *curPtr = strstr(strList, ":");
   if (*curPtr == NULL)
   {
      debug("no IPv6 info\n");
      *curPtr = strList;
      *nullPtr = NULL;
   }
   else
   {
      do
      {
         (*curPtr)--;
         if ((*curPtr) == strList)
         {
            debug("no IPv4 info\n");

            /* 
             * if there is no IPv4 info, the info file may still 
             * put an extra ',' in front of IPv6 info
             */
            if (**curPtr == ',')
            {
               *nullPtr = *curPtr + 1;
            }
            else
            {
               *nullPtr = strList;
            }
            *curPtr = NULL;

            break;
         }

         if (**curPtr == ',')
         {
            debug("IPv4 and IPv6 info are found\n");
            **curPtr = '\0';
            *nullPtr = *curPtr + 1;
            *curPtr = strList;
            break;
         }
      }
      while (1);
   }
}


UBOOL8 createDnsInfoEntry( DNSInfoEntry **ptr, 
                           struct in_addr *lanSubnet, struct in_addr *lanMask, 
                           char *ipv6prefix,
                           char *ipv4_dns1, char *ipv4_dns2, 
                           char *ipv6_dns1, char *ipv6_dns2, 
                           char *applicationBuf )
{
   UBOOL8 ret = FALSE;
   DNSInfoEntry *curr = NULL;
   UINT32 v4Dns = FALSE, v6Dns = FALSE;

   debug("create DnsInfoEntry: v4dns1<%s> v6dns1<%s>", 
                ipv4_dns1, ipv6_dns1);

   if ((lanSubnet == NULL) && (ipv6prefix == NULL))
   {
      cmsLog_error("No subnet info for IPv4 and IPv6");
      *ptr = NULL;
      ret = TRUE;
      goto end;
   }

   curr = cmsMem_alloc(sizeof(DNSInfoEntry), ALLOC_ZEROIZE);
   *ptr = curr;
   if (curr == NULL)
   {
      cmsLog_error("Failed to allocate memory.");
      ret = TRUE;
      goto end;
   }

   if (lanSubnet != NULL)
   {
      curr->lanSubnet  = *lanSubnet;
   }

   if (lanMask != NULL)
   {
      curr->lanMask = *lanMask;
   }

   curr->next = NULL;

   /* update IPv4 DNS server if available */
   if (!IS_EMPTY_STRING(ipv4_dns1) && cmsUtl_strcmp(ipv4_dns1, "0.0.0.0"))
   {
      curr->ipv4_dns1 = cmsMem_strdup(ipv4_dns1);
      curr->ipv4_dns2 = cmsMem_strdup(ipv4_dns2);
#if 1//__CTLK__, Fritz, modify dns swap mechanism
      curr->orig_ipv4_dns1 = cmsMem_strdup(ipv4_dns1);
#endif
      v4Dns = TRUE;
   }
   else
   {
      debug("No IPv4 DNS server");
   }

   /* update IPv6 DNS server if available */
   if ( !IS_EMPTY_STRING(ipv6_dns1) && 
        cmsUtl_isValidIpAddress(AF_INET6, ipv6_dns1) )
   {
      curr->ipv6_dns1 = cmsMem_strdup(ipv6_dns1);
      curr->ipv6_dns2 = cmsMem_strdup(ipv6_dns2);
#if 1//__CTLK__, Fritz, modify dns swap mechanism
      curr->orig_ipv6_dns1 = cmsMem_strdup(ipv6_dns1);
#endif
      v6Dns = TRUE;
   }
   else
   {
      debug("No IPv6 DNS server");
   }

   if (!v4Dns && !v6Dns)
   {
      cmsLog_error("NO DNS servers associated with DNSEntry");
      ret = TRUE;
      goto end;
   }

   if (ipv6prefix != NULL)
   {
      cmsUtl_strncpy( curr->lanIpv6Subnet[0], ipv6prefix, 
                      sizeof(curr->lanIpv6Subnet[0])-1 );
   }

   if (!IS_EMPTY_STRING(applicationBuf))
   {
      curr->processNameList = cmsMem_strdup(applicationBuf);
   }

   /* append the new node to the end of the linked list */
   if (head == NULL)
   {
      head = curr;
   }
   else
   {
      DNSInfoEntry *tmp = head;
      while (tmp->next != NULL)
      {
         tmp = tmp->next;
      }
      tmp->next = curr;
   }

end:
   return ret;
}


/* Initialize the global DNSInfoEntry linked list by parsing the information in \var\dnsinfo.conf created by CMS when there
* are wan connection status changes. 
*/
void dns_mapping_conifg_init(void)
{
   FILE *fp;
   char wanIfName[CMS_IFNAME_LENGTH] = {0};
   char subnetCidr[INET6_ADDRSTRLEN] = {0};
   char dnsList[5 * INET6_ADDRSTRLEN] = {0};   /* make space for 5 dns entries in case  they have that many */
   char ipv4_dns1[CMS_IPADDR_LENGTH];
   char ipv4_dns2[CMS_IPADDR_LENGTH];
   char ipv6_dns1[INET6_ADDRSTRLEN];
   char ipv6_dns2[INET6_ADDRSTRLEN];
   char line[BUFLEN_512]; // Enlarge the buffer size
   char applicationBuf[BUFLEN_64];
   DNSInfoEntry *curr = NULL;
   DNSInfoEntry *curr_tmp = NULL;
   UBOOL8 done = FALSE;
   struct in_addr lanSubnet;
   struct in_addr lanMask;
   char *curPtr;
   char *nullPtr;
   
   if ((fp = fopen(DNSINFO_CONF, "r")) == NULL)
   {
      cmsLog_notice(" %s does not exist.", DNSINFO_CONF);
      return;
   }

   debug("Enter!!");   
   /* First free the linked list */
   curr = head;
   while (curr)
   {
      cmsLog_notice("Free subnet=%X, mask=%X, ipv4_dns1=%s, ipv4_dns2=%s"
                    "ipv6_dns1=%s, ipv6_dns2=%s",
                    curr->lanSubnet, curr->lanMask, curr->ipv4_dns1, 
                    curr->ipv4_dns2, curr->ipv6_dns1, curr->ipv6_dns2);
      cmsMem_free(curr->ipv4_dns1);
      cmsMem_free(curr->ipv4_dns2);
      cmsMem_free(curr->ipv6_dns1);
      cmsMem_free(curr->ipv6_dns2);
      cmsMem_free(curr->processNameList);
#if 1 // __CTLK__, SeanLu 20150718, Fix memory leakage
      cmsMem_free(curr->orig_ipv4_dns1);
      cmsMem_free(curr->orig_ipv6_dns1);
#endif
      curr_tmp = curr;
      curr = curr->next;
      cmsMem_free(curr_tmp);
   }
   head = NULL;
   
   while (!done &&  fgets(line, sizeof(line), fp))
   {
      UBOOL8 v4Dns = FALSE, v6Dns = FALSE;

      curPtr = line;
      ipv6_dns1[0] = '\0';
      ipv6_dns2[0] = '\0';

      /* get rid of '\n' at the end if there is any */
      if (line[strlen(line) -1] == '\n')
      {
         line[strlen(line) - 1] = '\0';
      }

      /* 1) get Wan IfName */
      if ((nullPtr = strchr(curPtr, ';')) != NULL)
      {
         *nullPtr = '\0';
         strcpy(wanIfName, curPtr);
         curPtr = nullPtr + 1;         
      }

      /* 2) get subnet in cidr format */
      if ((nullPtr = strchr(curPtr, ';')) != NULL)
      {
         *nullPtr = '\0';
         strcpy(subnetCidr, curPtr);
         curPtr = nullPtr + 1;         
      }

      /* 3)  get dns list separated by ',' */
      if ((nullPtr = strchr(curPtr, ';')) != NULL)
      {
         *nullPtr = '\0';
         strcpy(dnsList, curPtr);
         curPtr = nullPtr + 1;         
      }

      /* 4) get apps list separated by ','
       * more than 1 applications such as "...;tr69c,voipd", etc on the WAN interface.
       */
      cmsUtl_strncpy(applicationBuf, curPtr, sizeof(applicationBuf)-1);

      
      cmsLog_notice("wanif=%s, subnetCidr=%s, dnsList=%s, processNameList=%s", 
                    wanIfName,  subnetCidr, dnsList, applicationBuf);

      /* 
       * process DNS part: curPtr points to the IPv4 DNS servers and nullPtr 
       * points to the IPv6 DNS servers
       */
      updateIpv4Ipv6Ptr(dnsList, &curPtr, &nullPtr);

      if (curPtr)
      {
         if (cmsUtl_parseDNS(curPtr,ipv4_dns1,ipv4_dns2,TRUE) != CMSRET_SUCCESS)
         {
            cmsLog_error("Failed to parse IPv4 dns list %s", dnsList);
         }
         else
         {
            v4Dns = TRUE;
         }
      }

      if (nullPtr)
      {
         if (cmsUtl_parseDNS(nullPtr,ipv6_dns1,ipv6_dns2,FALSE)!=CMSRET_SUCCESS)
         {
            cmsLog_error("Failed to parse IPv6 dns list %s", dnsList);
         }
         else
         {
            v6Dns = TRUE;
         }
      }

      if (!v4Dns && !v6Dns)
      {
         cmsLog_error("No DNS server info for DnsInfoEntry!!");
         fclose(fp);
         return;
      }

      /* 
       * process subnet part: curPtr points to the IPv4 subnet and nullPtr 
       * points to the IPv6 subnet
       *
       * IPv4 and IPv6 subnet may not exist concurrently. However, one of them
       * must be present! We have to update the DNSEntry with valid IPv4 AND
       * IPv6 DNS server info whenever a subnet is seen.
       */
      if (!IS_EMPTY_STRING(subnetCidr))
      {
         updateIpv4Ipv6Ptr(subnetCidr, &curPtr, &nullPtr);

         debug("v4Subnet<%s>  v6Subnet<%s>", curPtr, nullPtr);
         /*
          * Here we make an assumption: The configuration file must be 
          * correct. There will be no duplicate subnet in different entries.
          */
         if (curPtr)
         {
            /* case of IPv4 subnet */
            cmsNet_inet_cidrton(curPtr, &lanSubnet, &lanMask);
            if (lanSubnet.s_addr != 0 && lanMask.s_addr != 0)
            {
               done = createDnsInfoEntry(&curr, &lanSubnet, &lanMask, NULL,
                                         ipv4_dns1, ipv4_dns2, 
                                         ipv6_dns1, ipv6_dns2, applicationBuf);

               if (done)
               {
                  if ( curr )
                  {
                     cmsMem_free(curr);
                  }

                  fclose(fp);
                  return;
               }
            }
            else
            {
               cmsLog_error("Failed to convert LAN IPv4 subnet/mask from %s.", 
                            curPtr);
               done = TRUE;
            }
         }
         else
         {
            debug("No LAN IPv4 subnet/mask found.");
         }

         if (!done && nullPtr)
         {
            UINT32 plen;
            char addr[INET6_ADDRSTRLEN];
            char *tmp;

            /* 
             * Currently, we only support at most 2 IPv6 subnets associated
             * with one DnsInfoEntry. In the future, if we want to increase
             * the number, modification will be needed.
             */
            tmp = strstr(nullPtr, ",");

            if (tmp != NULL)
            {
               *tmp = '\0';
               tmp++;
            }

            if (cmsUtl_parsePrefixAddress(nullPtr,addr,&plen) == CMSRET_SUCCESS)
            {
               debug("ipv6 prefix=%s plen=%d", addr, plen);

               if (curr == NULL)   /* No entry is created by IPv4 */
               {
                  done = createDnsInfoEntry(&curr, NULL, NULL, nullPtr,
                                          ipv4_dns1, ipv4_dns2, 
                                          ipv6_dns1, ipv6_dns2, applicationBuf);

                  if (done)
                  {
                     if ( curr )
                     {
                        cmsMem_free(curr);
                     }

                     fclose(fp);
                     return;
                  }
                  else
                  {
                     /* update second ipv6 subnet if exists */
                     if ( cmsUtl_parsePrefixAddress(tmp, addr, &plen) == 
                          CMSRET_SUCCESS )
                     {
                        cmsUtl_strncpy( curr->lanIpv6Subnet[1], tmp, 
                                        sizeof(curr->lanIpv6Subnet[0])-1 );
                     }
                  }
               }
               else
               {
                  /* 
                   * An entry is created already by IPv4. Therefore, all info
                   * must be configured except for ipv6 subnet info
                   */
                  cmsUtl_strncpy( curr->lanIpv6Subnet[0], nullPtr, 
                                  sizeof(curr->lanIpv6Subnet[0])-1 );

                  if ( cmsUtl_parsePrefixAddress(tmp, addr, &plen) == 
                       CMSRET_SUCCESS )
                  {
                     cmsUtl_strncpy( curr->lanIpv6Subnet[1], tmp, 
                                     sizeof(curr->lanIpv6Subnet[0])-1 );
                  }
               }
            }
            else
            {
               cmsLog_error("Failed to convert LAN IPv6 subnet/mask from %s.", 
                            nullPtr);
               done = TRUE;
            }
         }
         else
         {
            debug("No LAN IPv6 prefix found.");
         }
      }
      else
      {
         cmsLog_error("No subnet info associated with a DNSEntry");
         done = TRUE;
      }

      wanIfName[0] = '\0';
      subnetCidr[0]= '\0';
      dnsList[0] = '\0';
   }

   curr = head;
   while (curr != NULL)
   {
      cmsLog_notice("subnet=%s", inet_ntoa(curr->lanSubnet));
      cmsLog_notice("mask=%s, dns1=%s, dns2=%s, processNameList=%s"
                    "v6prefix1=%s, v6prefix2=%s, v6dns1=%s, v6dns2=%s", 
                    inet_ntoa(curr->lanMask), curr->ipv4_dns1, curr->ipv4_dns2,
                    curr->processNameList, curr->lanIpv6Subnet[0], 
                    curr->lanIpv6Subnet[1], curr->ipv6_dns1, curr->ipv6_dns2);
      curr = curr->next;
   }

   fclose(fp);

#if 1 //__CTLK__, JhihPing
	char domain[BUFLEN_256];
	int cache;
	char lan[CMS_IPADDR_LENGTH];

   if ((fp = fopen("/var/fyi/sys/dns_config", "r")) == NULL)
   {
      debug("Opend /var/fyi/sys/dns_config failed");
      return 0;
	}

	while (fgets(line, sizeof(line), fp)) 
   {
      if (sscanf(line, "domain %s", domain) == 1)
      {
			strncpy(config.domain_name, domain, sizeof(config.domain_name) - 1);
		}
      else if (sscanf(line, "cache %d", &cache) == 1) 
      {
         config.cache = cache;
      }
      else if (sscanf(line, "LAN %s", lan) == 1)
      {
         strncpy(config.lan_ip, lan, sizeof(config.lan_ip) - 1);
      }
      else if (sscanf(line, "SubnetMask %s", lan) == 1)
      {
         strncpy(config.subnet_mask, lan, sizeof(config.subnet_mask) - 1);
      }
	}
	fclose(fp);
#endif

}


/* 
*  From lan ip and source port of the dns query, maps a correct wan dns ip for it.
*  Map a wan dns with the following steps:
* 1). If not loopback 127.0.0.1 and lan subnet matched, such as default and interface groups wan connections.
* 2). If local generated dns querry (tr69c, voip, etc.  the lan ip will be 127.0.0.1), use the source port and
*      process name to find out if the query is from this process or not.
* 3). If not match found, use the default dns (the first in the linked list).
*/
#if 1//__CTLK__, Fritz, modify dns swap mechanism
UBOOL8 dns_mapping_find_dns_ip(struct sockaddr_storage *lanInfo, int queryType, char *dns1, int *proto, UBOOL8 checkProtoSwap)
#else
UBOOL8 dns_mapping_find_dns_ip(struct sockaddr_storage *lanInfo, int queryType, char *dns1, int *proto)
#endif
{
   UBOOL8 found = FALSE;
   DNSInfoEntry *curr = head;
   char lanIP[INET6_ADDRSTRLEN];
   void *addr;
   int srcPort;
   
   if (dns1 == NULL)
   {
      cmsLog_error("Invalid dns1 parameter");
      return found;
   }

   addr = get_in_addr(lanInfo);
   srcPort = ((struct sockaddr_in *)lanInfo)->sin_port;
   inet_ntop(lanInfo->ss_family, addr, lanIP, INET6_ADDRSTRLEN);
   debug("lanIP=%s, srcPort=%d", lanIP, srcPort);

   while (!found && curr)
   {
      if (lanInfo->ss_family == AF_INET )
      {
         if (((struct in_addr *)addr)->s_addr != INADDR_LOOPBACK) 
         {
            if ( (((struct in_addr *)addr)->s_addr & curr->lanMask.s_addr) == 
                 curr->lanSubnet.s_addr )
            {
               /* 1).  Not loopback and same subnet found and need to get the 
                * dns1 (primary dns) 
                */
               found = findDnsServerAddress(curr, queryType, dns1, proto, checkProtoSwap);
               if (!found)
               {
                  cmsLog_error( "1). No IPv4/IPv6 dns server associate "
                                "with the entry" );
                  return found;
               }

               debug("1).  Found dns %s for subnet %s", dns1, lanIP);
            }            
         }
         else if (((struct in_addr *)addr)->s_addr == INADDR_LOOPBACK)
         {
           /* For applications such as tr69c, etc. which uses a specific 
            * routed wan service with an process
            * name associated with the wan connection in /var/dnsinfo.conf
            */
            if (isSourcePortMatchProcess(srcPort, curr->processNameList, TRUE))
            {
               found = findDnsServerAddress(curr, queryType, dns1, proto, checkProtoSwap);
               if (!found)
               {
                  cmsLog_error( "1). No IPv4/IPv6 dns server associate "
                                "with the entry" );
                  return found;
               }

               debug("2). Found the dns %s for the application", dns1);
            }
         }
         else
         {
            cmsLog_error("Invalid lanIP %s", lanIP);
         }
      }
      else /* AF_INET6 */
      {
         if ( !IN6_ARE_ADDR_EQUAL(addr, &in6addr_loopback) )
         {
            if ( cmsNet_isHostInSameSubnet(lanIP, curr->lanIpv6Subnet[0]) ||
                 cmsNet_isHostInSameSubnet(lanIP, curr->lanIpv6Subnet[1]) )
            {
               /* 1).  Not loopback and same subnet found and need to get the 
                * dns1 (primary dns) 
                */
               found = findDnsServerAddress(curr, queryType, dns1, proto, checkProtoSwap);
               if (!found)
               {
                  cmsLog_error( "1). No IPv4/IPv6 dns server associate "
                                "with the entry" );
                  return found;
               }

               debug("1).  Found dns %s for subnet %s", dns1, lanIP);
            }
         }
         else if ( IN6_ARE_ADDR_EQUAL(addr, &in6addr_loopback) )
         {
           /* For applications such as tr69c, etc. which uses a specific 
            * routed wan service with an process
            * name associated with the wan connection in /var/dnsinfo.conf
            */
            if (isSourcePortMatchProcess(srcPort, curr->processNameList, FALSE))
            {
               found = findDnsServerAddress(curr, queryType, dns1, proto, checkProtoSwap);
               if (!found)
               {
                  cmsLog_error( "1). No IPv4/IPv6 dns server associate "
                                "with the entry" );
                  return found;
               }

               debug("2). Found the dns %s for the application", dns1);
            }
         }
         else
         {
            cmsLog_error("Invalid lanIP %s", lanIP);
         }
      }

      if (!found)
      {
         curr = curr->next;
         debug("Not found. get next one");
      }         
   }

   if (!found)
   {
      /* If not found in the above while loop, use the default system dns (the first one in the linked list) */
      curr = head;
      if (curr)
      {
         found = findDnsServerAddress(curr, queryType, dns1, proto, checkProtoSwap);
         debug("3).  Use the default dns.");
      }
   }

   return found;
   
 }
 
 UBOOL8 dns_mapping_is_dns_swap_needed(dns_request_t *r, char *dnsUsed)
{
   DNSInfoEntry *curr = head;
   char currDns[INET6_ADDRSTRLEN];
   int proto;
   UBOOL8 isSwapNeeded = FALSE;

  
  if (NULL == r || dnsUsed == NULL) 
  {
     cmsLog_error("null request passed in!");
     return isSwapNeeded;
  }

   cmsLog_notice("Enter dnsUsed %s", dnsUsed);

#if 1//__CTLK__, Fritz, modify dns swap mechanism
   if (!dns_mapping_find_dns_ip(&(r->src_info), r->message.question[0].type, currDns, &proto, FALSE))
#else
   if (!dns_mapping_find_dns_ip(&(r->src_info), r->message.question[0].type, currDns, &proto))
#endif
   {
      cmsLog_notice("Cannot find the dns ip used for the query.  Just remove this request.");
      isSwapNeeded = TRUE;
      return isSwapNeeded;
   }

   while (curr && !isSwapNeeded)
   {
      /* Both dns1 and dns2 need to be present and curr->ipv4_dns1 is always used */
      if (proto == AF_INET && curr->ipv4_dns1 && curr->ipv4_dns2)
      {
         if (!cmsUtl_strcmp(currDns, curr->ipv4_dns1))
         { 
            if (IS_EMPTY_STRING(dnsUsed))
            {
               strcpy(dnsUsed, curr->ipv4_dns1);
               isSwapNeeded = TRUE;
            }               
            else if (!cmsUtl_strcmp(curr->ipv4_dns1, dnsUsed))
            {
                isSwapNeeded = TRUE;
                curr->isSwapped = TRUE;
            }
            else
            {
               cmsLog_notice("Different dns1 used for the query so skip removal");
            }
         }
      }
      else if (proto == AF_INET6 && curr->ipv6_dns1 && curr->ipv6_dns2)
      {
         /* Both dns1 and dns2 need to be present and curr->ipv6_dns1 is always used */      
         if (!cmsUtl_strcmp(currDns, curr->ipv6_dns1))
         {             
            if (IS_EMPTY_STRING(dnsUsed))
            {
               strcpy(dnsUsed, curr->ipv6_dns1);
               isSwapNeeded = TRUE;
            }               
            else if (!cmsUtl_strcmp(curr->ipv6_dns1, dnsUsed))
            {
                isSwapNeeded = TRUE;
                curr->isSwapped = TRUE;
            }
            else
            {
               cmsLog_notice("Different dns1 used for the query so skip removal");
            }
         }
      }
         
      curr = curr->next;
   }

   return isSwapNeeded;
   
}


void dns_mapping_do_dns_swap(char *dnsUsed)
{
   DNSInfoEntry *curr = head;
   UBOOL8 swapDone = FALSE;
   char tmpDns1[INET6_ADDRSTRLEN];
   
   while (curr && !swapDone)
   {
      if (curr->ipv4_dns1 && curr->ipv4_dns2 && !cmsUtl_strcmp(dnsUsed, curr->ipv4_dns1))
      {             
         strcpy(tmpDns1, curr->ipv4_dns1);
         CMSMEM_REPLACE_STRING(curr->ipv4_dns1, curr->ipv4_dns2);
         CMSMEM_REPLACE_STRING(curr->ipv4_dns2, tmpDns1);
         swapDone = TRUE;
         cmsLog_notice("After swap, swap ipv4_dns1=%s, ipv4_dns2=%s", curr->ipv4_dns1,  curr->ipv4_dns2);
      }
      else if ( curr->ipv6_dns1 && curr->ipv6_dns2 && !cmsUtl_strcmp(dnsUsed, curr->ipv6_dns1))
      {             
         strcpy(tmpDns1, curr->ipv6_dns1);
         CMSMEM_REPLACE_STRING(curr->ipv6_dns1, curr->ipv6_dns2);
         CMSMEM_REPLACE_STRING(curr->ipv6_dns2, tmpDns1);
         swapDone = TRUE;
         cmsLog_notice("After swap, ipv6_dns1=%s, ipv6_dns2=%s", curr->ipv6_dns1, curr->ipv6_dns2);
      }
      curr = curr->next;
   }

}
