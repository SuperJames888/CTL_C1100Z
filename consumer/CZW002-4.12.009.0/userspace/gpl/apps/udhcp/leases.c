/* 
 * leases.c -- tools to manage DHCP leases 
 * Russ Dill <Russ.Dill@asu.edu> July 2001
 */

#include <time.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "debug.h"
#include "dhcpd.h"
#include "files.h"
#include "options.h"
#include "leases.h"
#include "arpping.h"

//For static IP lease
#include "static_leases.h"

/* clear every lease out that chaddr OR yiaddr matches and is nonzero */
void clear_lease(u_int8_t *chaddr, u_int32_t yiaddr)
{
	unsigned int i, blank_chaddr = 0, blank_yiaddr = 0;
	
	for (i = 0; i < 16 && !chaddr[i]; i++);
	if (i == 16) blank_chaddr = 1;
	blank_yiaddr = (yiaddr == 0);
	
	for (i = 0; i < cur_iface->max_leases; i++)
		if ((!blank_chaddr && !memcmp(cur_iface->leases[i].chaddr,
			chaddr, 16)) ||
		    (!blank_yiaddr && cur_iface->leases[i].yiaddr == yiaddr)) {
			memset(&(cur_iface->leases[i]), 0,
				sizeof(struct dhcpOfferedAddr));
		}
}


/* add a lease into the table, clearing out any old ones */
#if 1 //__CTLK__, Eric, merge from Q1000Z
struct dhcpOfferedAddr *add_lease(u_int8_t *chaddr, u_int32_t yiaddr, unsigned long lease, u_int8_t isStatic)
{
	struct dhcpOfferedAddr *oldest;
	
	/* clean out any old ones */
	clear_lease(chaddr, yiaddr);
		
	oldest = oldest_expired_lease();
	
	if (oldest) {
		memcpy(oldest->chaddr, chaddr, 16);
		oldest->yiaddr = yiaddr;
		oldest->expires = time(0) + lease;
		oldest->isStatic = isStatic;
	}
	return oldest;
}

#else
struct dhcpOfferedAddr *add_lease(u_int8_t *chaddr, u_int32_t yiaddr, unsigned long lease)
{
	struct dhcpOfferedAddr *oldest;
	
	/* clean out any old ones */
	clear_lease(chaddr, yiaddr);
		
	oldest = oldest_expired_lease();
	
	if (oldest) {
		memcpy(oldest->chaddr, chaddr, 16);
		oldest->yiaddr = yiaddr;
		oldest->expires = time(0) + lease;
	}
	
	return oldest;
}
#endif

/* true if a lease has expired */
int lease_expired(struct dhcpOfferedAddr *lease)
{
	return (lease->expires < (unsigned long) time(0));
}	


/* return the number of seconds left in the lease */
int lease_time_remaining(const struct dhcpOfferedAddr *lease)
{
   unsigned long now = (unsigned long) time(0);

   if (lease->expires > now) {
      return (lease->expires - now);
   }
   else {
      return 0;
   }
}

/* Find the oldest expired lease, NULL if there are no expired leases */
struct dhcpOfferedAddr *oldest_expired_lease(void)
{
	struct dhcpOfferedAddr *oldest = NULL;
	unsigned long oldest_lease = time(0);
	unsigned int i;

	
	for (i = 0; i < cur_iface->max_leases; i++)
		if (oldest_lease > cur_iface->leases[i].expires) {
			oldest_lease = cur_iface->leases[i].expires;
			oldest = &(cur_iface->leases[i]);
		}
	return oldest;
		
}


/* Find the first lease that matches chaddr, NULL if no match */
struct dhcpOfferedAddr *find_lease_by_chaddr(u_int8_t *chaddr)
{
	unsigned int i;

	for (i = 0; i < cur_iface->max_leases; i++)
		if (!memcmp(cur_iface->leases[i].chaddr, chaddr, 16))
			return &(cur_iface->leases[i]);
	
	return NULL;
}


/* Find the first lease that matches yiaddr, NULL is no match */
struct dhcpOfferedAddr *find_lease_by_yiaddr(u_int32_t yiaddr)
{
	unsigned int i;

	for (i = 0; i < cur_iface->max_leases; i++)
		if (cur_iface->leases[i].yiaddr == yiaddr)
			return &(cur_iface->leases[i]);
	
	return NULL;
}


/* find an assignable address, it check_expired is true, we check all the expired leases as well.
 * Maybe this should try expired leases by age... */
#if 0 //__CTLK__, Thief
u_int32_t find_address(int check_expired) 
{
#else
u_int32_t find_address(int check_expired, int check_active)
{
	if(!check_active) {
#endif
	u_int32_t addr, ret = 0;
	struct dhcpOfferedAddr *lease = NULL;

	addr = cur_iface->start;
	// brcm
	for (;ntohl(addr) <= ntohl(cur_iface->end);
		addr = htonl(ntohl(addr) + 1)) {

		/* ie, 192.168.55.0 */
		if (!(ntohl(addr) & 0xFF)) continue;

		/* ie, 192.168.55.255 */
		if ((ntohl(addr) & 0xFF) == 0xFF) continue;

		//For static IP lease
		/* Only do if it isn't an assigned as a static lease */
		if(!reservedIp(cur_iface->static_leases, htonl(addr))) 
		{
		/* lease is not taken */
		   ret = htonl(addr);
		   if ((!(lease = find_lease_by_yiaddr(ret)) ||
		     	/* or it expired and we are checking for expired leases */
		    	 (check_expired  && lease_expired(lease))) &&
		   		/* and it isn't on the network */
	    	     !check_ip(ret)) 	    	     
		   {
		      return ret;
		   }
		}
	}
#if 1 //__CTLK__, Thief
	}
	else { /*find a lease whose device doesn't active*/
		struct static_lease *cur = cur_iface->static_leases;
#if 1//YCT ,DHCP reservation ,refer to C1000Z	
	if(!cur_iface->auto_reservation)
	{
#endif
	    while(cur != NULL)
	    {
		if(!check_ip(*cur->ip)){
			
				char delarpentry[BUFLEN_32] = {0};
				struct in_addr temp;
				memset(&temp, 0, sizeof(temp));

				temp.s_addr = *cur->ip;
				sprintf(delarpentry, "ip neigh flush %s", inet_ntoa(temp)); /*Clear ARP Entry*/
				system(delarpentry);

                		return *cur->ip;
			}

		    cur = cur->next;
	    }
#if 1//YCT ,DHCP reservation ,refer to C1000Z	
	}
	else
	{
	
		u_int32_t addr, ret = 0;
		struct dhcpOfferedAddr *lease = NULL;
		addr = cur_iface->start;
		// brcm
		for (;ntohl(addr) <= ntohl(cur_iface->end);
			addr = htonl(ntohl(addr) + 1)) {

			/* ie, 192.168.55.0 */
			if (!(ntohl(addr) & 0xFF)) continue;

			/* ie, 192.168.55.255 */
			if ((ntohl(addr) & 0xFF) == 0xFF) continue;

			//For static IP lease
			/* Only do if it isn't an assigned as a static lease */

		if(!reservedIp(cur_iface->static_leases, htonl(addr))) 
		{
			/* lease is not taken */
		   	ret = htonl(addr);

		   	if (!check_ip(ret)) 	     
		   	{
		      		return ret;
		   	}
		}
	    }
	}
#endif
	}
#endif
	return 0;
}


/* return a pointer to the iface struct that has the specified interface name, e.g. br0 */
struct iface_config_t *find_iface_by_ifname(const char *name)
{
   struct iface_config_t *iface;

   for (iface = iface_config; iface; iface = iface->next) {
      if (!cmsUtl_strcmp(iface->interface, name))
      {
         return iface;
      }
   }

   return NULL;
}


/* check is an IP is taken, if it is, add it to the lease table */
int check_ip(u_int32_t addr)
{
	char blank_chaddr[] = {[0 ... 15] = 0};
	struct in_addr temp;

//brcm
#if 1 //__CTLK__,JhenYang,DHCP:sync from SinJia, TR-098 DHCP Conditional Serving Pool
	if (!arpping(addr, cur_iface->server, 
		cur_iface->UsedIf))
#else
	if (!arpping(addr, cur_iface->server, 
		cur_iface->interface)) 
#endif
	{
		temp.s_addr = addr;
	 	LOG(LOG_INFO, "%s belongs to someone, reserving it for %ld seconds", 
	 		inet_ntoa(temp), server_config.conflict_time);
#if 1 //__CTLK__, Eric, merge from Q1000Z
		add_lease(blank_chaddr, addr, server_config.conflict_time, FALSE);
#else
		add_lease(blank_chaddr, addr, server_config.conflict_time);
#endif
		return 1;
	} else return 0;
}

void adjust_lease_time(long delta)
{
	struct iface_config_t * iface;
	unsigned int i;

	cur_iface = iface_config;
	while(cur_iface) {
	    iface = cur_iface->next;
	    for (i = 0; i < cur_iface->max_leases && cur_iface->leases[i].expires; i++) {
	        cur_iface->leases[i].expires += delta;
	        }
	    cur_iface = iface;
	}
}
#if 1 //__CTLK__, Eric, merge from Q1000Z
void checkStaticHosts(u_int8_t *chaddr)
{
   struct dhcpOfferedAddr *staticHost = NULL;
   struct option_set *subnet_option;
   struct iface_config_t *temp_iface;
   unsigned int subnet_mask;
   char *ptr;
//   char temp[20];

   temp_iface = cur_iface;
   for (cur_iface = iface_config ; cur_iface ; cur_iface = cur_iface->next)
   {
      for (cur_arp = arp_head ; cur_arp != NULL ; cur_arp = cur_arp->next)
      {
//         cmsUtl_macNumToStr(cur_arp->chaddr, temp);
         staticHost = find_lease_by_chaddr(cur_arp->chaddr);

         if (!memcmp(cur_arp->chaddr, chaddr, 16)) {
            if (staticHost && staticHost->isStatic)
               clear_lease(staticHost->chaddr, staticHost->yiaddr);
            continue; // get the same mac address
         }

         if (staticHost)
         {
            if (staticHost->yiaddr == cur_arp->yiaddr)
               continue;
            else
            {
               add_lease(cur_arp->chaddr, cur_arp->yiaddr, server_config.offer_time, TRUE);
               strcpy(staticHost->hostname, "unknown");
               send_lease_info(FALSE, staticHost);
            }
         }
         else
         {
            subnet_option = find_option(cur_iface->options, 1);
            ptr = subnet_option->data;
            subnet_mask = (*(ptr + 2) & 0xff) << 24 | (*(ptr + 3) & 0xff) << 16 | (*(ptr + 4) & 0xff) << 8 | (*(ptr + 5) & 0xff);

            if ((cur_arp->yiaddr & subnet_mask) == (cur_iface->server & subnet_mask))
            {
               add_lease(cur_arp->chaddr, cur_arp->yiaddr, server_config.offer_time, TRUE);
               staticHost = find_lease_by_chaddr(cur_arp->chaddr);
			   if (staticHost)
               	strcpy(staticHost->hostname, "unknown");

               if (!staticHost)
                  fprintf(stderr, "checkStaticHost error\n");
               else {
                  send_lease_info(FALSE, staticHost);
               }
            }
         }
      }
   }

   cur_iface = temp_iface;
   return;
}

void handleHostInfoUpdate(CmsMsgHeader *msg)
{
   DhcpdHostInfoMsgBody *body = (DhcpdHostInfoMsgBody *) (msg + 1);
   struct dhcpOfferedAddr *leaseHost = NULL;
   u_int8_t chaddr[16];
   u_int32_t yiaddr;
   struct in_addr inaddr;

   cur_iface = find_iface_by_ifname(body->ifName);
   memset(chaddr, 0, sizeof(chaddr));
   cmsUtl_macStrToNum(body->macAddr, chaddr);
   leaseHost = find_lease_by_chaddr(chaddr);
   inet_aton(body->ipAddr, &inaddr);
   yiaddr = inaddr.s_addr;

   if (leaseHost)
   {
      if (body->deleteHost)
      {
         clear_lease(chaddr, yiaddr);
      }
      else
      {
         add_lease(chaddr, yiaddr, server_config.offer_time, TRUE);
      }
   }
   else
   {
      if (body->deleteHost)
      {
         fprintf(stderr, "delete lease error\n");
      }
      else
      {
         add_lease(chaddr, yiaddr, server_config.offer_time, TRUE);
         leaseHost = find_lease_by_chaddr(chaddr);
         if (!leaseHost)
         { 
            fprintf(stderr, "add lease error\n");
         }
      }
   }

   return;
}
#endif

