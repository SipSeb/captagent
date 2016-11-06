/*
 * $Id$
 *
 *  captagent - Homer capture agent. Modular
 *  Duplicate SIP messages in Homer Encapulate Protocol [HEP] [ipv6 version]
 *
 *  Author: Alexandr Dubovikov <alexandr.dubovikov@gmail.com>
 *  (C) Homer Project 2012-2015 (http://www.sipcapture.org)
 *
 * Homer capture agent is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version
 *
 * Homer capture agent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>

#ifndef __FAVOR_BSD
#define __FAVOR_BSD
#endif /* __FAVOR_BSD */

#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#ifdef USE_IPV6
#include <netinet/ip6.h>
#endif /* USE_IPV6 */

#include <captagent/capture.h>
#include <captagent/globals.h>
#include <captagent/api.h>
#include <captagent/proto_sip.h>
#include <captagent/structure.h>
#include <captagent/modules_api.h>
#include "socket_tzsp.h"
#include <captagent/modules.h>
#include <captagent/log.h>
#include <captagent/action.h>

profile_socket_t profile_socket[MAX_SOCKETS];

xml_node *module_xml_config = NULL;

uint8_t link_offset = 14;

char *module_name="socket_tzsp";
uint64_t module_serial = 0;
char *module_description;

uv_loop_t *loop;
uv_thread_t runthread;
uv_async_t async_handle;
static uv_udp_t  udp_servers[MAX_SOCKETS];
int reply_to_tzsp = 1;
static socket_tzsp_stats_t stats;
int verbose = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t call_thread;
struct reasm_ip *reasm[MAX_SOCKETS];

static int load_module(xml_node *config);
static int unload_module(void);
static int description(char *descr);
static int statistic(char *buf, size_t len);
static uint64_t serial_module(void);
static int free_profile(unsigned int idx);

unsigned int profile_size = 0;

bind_protocol_module_api_t proto_bind_api;

static cmd_export_t cmds[] = {
	{"socket_tzsp_bind_api", (cmd_function) bind_api, 1, 0, 0, 0 },
	{"tzsp_payload_extract", (cmd_function) w_tzsp_payload_extract, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0 }
};

struct module_exports exports = {
        "socket_tzsp",
        cmds,        /* Exported functions */
        load_module,    /* module initialization function */
        unload_module,
        description,
        statistic,
        serial_module
};

int bind_api(socket_module_api_t* api)
{
	api->reload_f = reload_config;
	api->module_name = module_name;
        return 0;
}

int reload_config (char *erbuf, int erlen) {

	char module_config_name[500];
	xml_node *config = NULL;

	LNOTICE("reloading config for [%s]", module_name);

	snprintf(module_config_name, 500, "%s/%s.xml", global_config_path, module_name);

	if(xml_parse_with_report(module_config_name, erbuf, erlen)) {
		unload_module();
		load_module(config);
		return 1;
	}

	return 0;
}

static inline const char* name_tag(int tag,
                                   const char * const names[],
                                   int names_len) {
	if (tag >= 0 && tag < names_len) {
		return names[tag];
	}
	else {
		return "<UNKNOWN>";
	}
}

static inline int max(int x, int y) {
	return (x > y) ? x : y;
}

void on_send(uv_udp_send_t* req, int status) 
{
	if (status == 0 && req) {
		free(req->data);
		free(req); 
		req = NULL;
	}
}
   
 
int w_tzsp_payload_extract(msg_t *_m)
{
        int readsz = 0;
        char *recv_buffer = NULL;

        recv_buffer = _m->data;
        readsz = _m->len;
        
        char *end = recv_buffer + readsz;
        char *p = recv_buffer;
        
        if (p + sizeof(struct tzsp_header) > end) 
        {
                LERR("Malformed packet (truncated header)");
                return -1;
        }
        
	struct tzsp_header *hdr = (struct tzsp_header *) recv_buffer;
	p += sizeof(struct tzsp_header);        
	
	char got_end_tag = 0;
	if (hdr->version == 1 && hdr->type == TZSP_TYPE_RECEIVED_TAG_LIST)
	{
		while (p < end) 
		{
			struct tzsp_tag *tag = (struct tzsp_tag *) p;

			if (verbose) LERR("\ttag { type = %s(%u) }", name_tag(tag->type, tzsp_tag_names, ARRAYSZ(tzsp_tag_names)), tag->type);

			if (tag->type == TZSP_TAG_END) 
			{
				got_end_tag = 1;
				p++;
				break;
			}
			else if (tag->type == TZSP_TAG_PADDING) {
				p++;
			}
			else {
				if (p + sizeof(struct tzsp_tag) > end || p + sizeof(struct tzsp_tag) + tag->length > end)
				{
					LERR("Malformed packet (truncated tag)");
					return -1;
				}
				p += sizeof(struct tzsp_tag) + tag->length;
			}
		}
	}
	else {
		LERR("Packet format not understood");
		return -1;
	}

	if (!got_end_tag) {
		LERR("Packet truncated (no END tag)");
		return -1;
	}
		
	if (verbose) {
		LERR("\tpacket data begins at offset 0x%.4lx, length 0x%.4lx\n",(p - recv_buffer),readsz - (p - recv_buffer));
	}

	// packet remains starting at p
	struct pcap_pkthdr pcap_hdr = {
		.caplen = readsz - (p - recv_buffer),
		.len = readsz - (p - recv_buffer),
	};
	gettimeofday(&pcap_hdr.ts, NULL);
	proccess_packet(_m,  &pcap_hdr, (unsigned char *) p);
 
        return 1;
}


void proccess_packet(msg_t *_m, struct pcap_pkthdr *pkthdr, u_char *packet) {

	uint8_t hdr_offset = 0;
	uint16_t ethaddr;
	uint16_t mplsaddr;

	/* Pat Callahan's patch for MPLS */
	memcpy(&ethaddr, (packet + 12), 2);
        memcpy(&mplsaddr, (packet + 16), 2);

        if (ntohs(ethaddr) == 0x8100) {
          if (ntohs(mplsaddr) == 0x8847) {
             hdr_offset = 8;
          } else {
             hdr_offset = 4;
          }
        }

        struct ethhdr *eth = (struct ethhdr *)packet;
        
        struct ip      *ip4_pkt = (struct ip *)    (packet + link_offset + hdr_offset);
#if USE_IPv6
        struct ip6_hdr *ip6_pkt = (struct ip6_hdr*)(packet + link_offset + hdr_offset + ((ntohs((uint16_t)*(packet + 12)) == 0x8100)? 4: 0) );
#endif

	uint32_t ip_ver;
	uint8_t ip_proto = 0;
	uint32_t ip_hl = 0;
	uint32_t ip_off = 0;
	uint8_t fragmented = 0;
	uint16_t frag_offset = 0;
	char ip_src[INET6_ADDRSTRLEN + 1], ip_dst[INET6_ADDRSTRLEN + 1];
	char mac_src[20], mac_dst[20];
	uint32_t len = pkthdr->caplen;
	        
	ip_ver = ip4_pkt->ip_v;

        snprintf(mac_src, sizeof(mac_src), "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X",eth->h_source[0] , eth->h_source[1] , eth->h_source[2] , eth->h_source[3] , eth->h_source[4] , eth->h_source[5]);
        snprintf(mac_dst, sizeof(mac_dst), "%.2X-%.2X-%.2X-%.2X-%.2X-%.2X", eth->h_dest[0] , eth->h_dest[1] , eth->h_dest[2] , eth->h_dest[3] , eth->h_dest[4] , eth->h_dest[5]);
        
        _m->cap_packet = (void *) packet;
        _m->cap_header = (void *) pkthdr;                

	switch (ip_ver) {

        	case 4: {
        #if defined(AIX)
#undef ip_hl
        		ip_hl = ip4_pkt->ip_ff.ip_fhl * 4;
#else
	        	ip_hl = ip4_pkt->ip_hl * 4;
#endif
		        ip_proto = ip4_pkt->ip_p;
        		ip_off = ntohs(ip4_pkt->ip_off);

	        	fragmented = ip_off & (IP_MF | IP_OFFMASK);
        		frag_offset = (fragmented) ? (ip_off & IP_OFFMASK) * 8 : 0;
	        	//frag_id = ntohs(ip4_pkt->ip_id);

	        	inet_ntop(AF_INET, (const void *) &ip4_pkt->ip_src, ip_src, sizeof(ip_src));
        		inet_ntop(AF_INET, (const void *) &ip4_pkt->ip_dst, ip_dst, sizeof(ip_dst));
                }
		break;

#if USE_IPv6
                case 6: {
	                ip_hl = sizeof(struct ip6_hdr);
	                ip_proto = ip6_pkt->ip6_nxt;

        		if (ip_proto == IPPROTO_FRAGMENT) {
	        	        struct ip6_frag *ip6_fraghdr;
		                ip6_fraghdr = (struct ip6_frag *)((unsigned char *)(ip6_pkt) + ip_hl);
		                ip_hl += sizeof(struct ip6_frag);
        			ip_proto = ip6_fraghdr->ip6f_nxt;
	        		fragmented = 1;
		        	frag_offset = ntohs(ip6_fraghdr->ip6f_offlg & IP6F_OFF_MASK);
        			//frag_id = ntohl(ip6_fraghdr->ip6f_ident);
                        }

                        inet_ntop(AF_INET6, (const void *)&ip6_pkt->ip6_src, ip_src, sizeof(ip_src));
        		inet_ntop(AF_INET6, (const void *)&ip6_pkt->ip6_dst, ip_dst, sizeof(ip_dst));
                }
                break;
#endif
	}

	switch (ip_proto) {

        	case IPPROTO_TCP: {
	        	struct tcphdr *tcp_pkt = (struct tcphdr *) ((unsigned char *) (ip4_pkt) + ip_hl);
        		uint16_t tcphdr_offset = frag_offset ? 0 : (uint16_t) (tcp_pkt->th_off * 4);        		
        		//data = (unsigned char *) tcp_pkt + tcphdr_offset;		
        		_m->hdr_len = link_offset + hdr_offset + ip_hl + tcphdr_offset;
        		len -= link_offset + hdr_offset + ip_hl + tcphdr_offset;

        		if ((int32_t) len < 0) len = 0;
        		
        		_m->len = pkthdr->caplen - link_offset - hdr_offset;
	        	_m->data = (packet + link_offset + hdr_offset);

	        	_m->rcinfo.src_port = ntohs(tcp_pkt->th_sport);
        		_m->rcinfo.dst_port = ntohs(tcp_pkt->th_dport);
	        	_m->rcinfo.src_ip = ip_src;
	        	_m->rcinfo.dst_ip = ip_dst;
        		_m->rcinfo.src_mac = mac_src;
	        	_m->rcinfo.dst_mac = mac_dst;
        		_m->rcinfo.ip_family = ip_ver == 4 ? AF_INET : AF_INET6;
        		_m->rcinfo.ip_proto = ip_proto;
        		//_m->rcinfo.time_sec = pkthdr->ts.tv_sec;
        		//_m->rcinfo.time_usec = pkthdr->ts.tv_usec;
        		_m->tcpflag = tcp_pkt->th_flags;
        		_m->parse_it = 1;        		
        	}
        	break;

        	case IPPROTO_UDP: {
	        	struct udphdr *udp_pkt = (struct udphdr *) ((unsigned char *) (ip4_pkt) + ip_hl);
        		uint16_t udphdr_offset = (frag_offset) ? 0 : sizeof(*udp_pkt);
	        	//data = (unsigned char *) (udp_pkt) + udphdr_offset;
		
        		_m->hdr_len = link_offset + ip_hl + hdr_offset + udphdr_offset;
	        	
        		len -= link_offset + ip_hl + udphdr_offset + hdr_offset;
				
	        	/* stats */
        		if ((int32_t) len < 0) len = 0;

	        	_m->len = pkthdr->caplen - link_offset - hdr_offset;
        		_m->data = (packet + link_offset + hdr_offset);
	
	        	_m->rcinfo.src_port = ntohs(udp_pkt->uh_sport);
        		_m->rcinfo.dst_port = ntohs(udp_pkt->uh_dport);
        		_m->rcinfo.src_ip = ip_src;
        		_m->rcinfo.dst_ip = ip_dst;
        		_m->rcinfo.src_mac = mac_src;
        		_m->rcinfo.dst_mac = mac_dst;
        		_m->rcinfo.ip_family = ip_ver == 4 ? AF_INET : AF_INET6;
        		_m->rcinfo.ip_proto = ip_proto;
        		//_m->rcinfo.time_sec = pkthdr->ts.tv_sec;
        		//_m->rcinfo.time_usec = pkthdr->ts.tv_usec;
        		_m->tcpflag = 0;
        		_m->parse_it = 1;        		
        	}
		break;
		
        	default:
	        	break;
        }
	
	return;
}



#if UV_VERSION_MAJOR == 0                         
void on_recv(uv_udp_t* handle, ssize_t nread, uv_buf_t rcvbuf, struct sockaddr* addr, unsigned flags)
#else
void on_recv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* rcvbuf, const struct sockaddr* addr, unsigned flags) 
#endif    
{
       
    msg_t _msg;
    struct timeval  tv;
    int action_idx = 0;
    struct run_act_ctx ctx;
    struct sockaddr_in *cliaddr;
    uint8_t loc_idx;

    if (nread <= 0 || addr == NULL) 
    {
#if UV_VERSION_MAJOR == 0                            
    	free(rcvbuf.base);
#else
        free(rcvbuf->base);
#endif       	
    	return;
    }

    loc_idx = *((uint8_t *) handle->data);
    
    gettimeofday(&tv, NULL);

    cliaddr = (struct sockaddr_in*)addr;

    memset(&_msg, 0, sizeof(msg_t));
    memset(&ctx, 0, sizeof(struct run_act_ctx));

#if UV_VERSION_MAJOR == 0                             
    _msg.data = rcvbuf.base;
#else
    _msg.data = rcvbuf->base;
#endif    

    _msg.len = nread;
    
    _msg.rcinfo.dst_port = ntohs(cliaddr->sin_port);
    _msg.rcinfo.dst_ip = inet_ntoa(cliaddr->sin_addr);
    _msg.rcinfo.liid = loc_idx;

    _msg.rcinfo.src_port = atoi(profile_socket[loc_idx].port);
    _msg.rcinfo.src_ip = profile_socket[loc_idx].host;

    _msg.rcinfo.ip_family = addr->sa_family;
    _msg.rcinfo.ip_proto = IPPROTO_UDP;
		
    _msg.rcinfo.proto_type = profile_socket[loc_idx].protocol;
    _msg.rcinfo.time_sec = tv.tv_sec;
    _msg.rcinfo.time_usec = tv.tv_usec;
    _msg.tcpflag = 0;
    _msg.parse_it = 0;
    _msg.rcinfo.socket = &profile_socket[loc_idx].socket;

    _msg.var = (void *) addr;
    _msg.flag[5] = loc_idx;

    action_idx = profile_socket[loc_idx].action;
    run_actions(&ctx, main_ct.clist[action_idx], &_msg);		                        
    
#if UV_VERSION_MAJOR == 0                            
    	free(rcvbuf.base);
#else
        free(rcvbuf->base);
#endif       	
}
 
#if UV_VERSION_MAJOR == 0                         
uv_buf_t on_alloc(uv_handle_t* client, size_t suggested) {
	char *chunk = malloc(suggested);
	memset(chunk, 0, suggested);
	return uv_buf_init(chunk, suggested);     	        
}

#else 
void on_alloc(uv_handle_t* client, size_t suggested, uv_buf_t* buf) {
    
	char *chunk = malloc(suggested);
	memset(chunk, 0, suggested);
	*buf = uv_buf_init(chunk, suggested);	      
}
#endif

#if UV_VERSION_MAJOR == 0
  void _async_callback(uv_async_t *async, int status)
#else
  void _async_callback(uv_async_t *async)
#endif
{
    LDEBUG("In async callback socket tzsp exit");
}


void _run_uv_loop(void *arg)
{
     uv_loop_t * myloop = (uv_loop_t *)arg;
     uv_run(myloop, UV_RUN_DEFAULT);
}
                 
int close_socket(unsigned int loc_idx) {         
	
	uv_udp_recv_stop(&udp_servers[loc_idx]);	  
	uv_close((uv_handle_t*)&udp_servers[loc_idx], NULL);	  
	return 0;
}
         
int init_socket(unsigned int loc_idx) {

	struct sockaddr_in v4addr;
	int status;

	status = uv_udp_init(loop,&udp_servers[loc_idx]);

#if UV_VERSION_MAJOR == 0                         
	v4addr = uv_ip4_addr(profile_socket[loc_idx].host, atoi(profile_socket[loc_idx].port));

#else    
      	status = uv_ip4_addr(profile_socket[loc_idx].host, atoi(profile_socket[loc_idx].port), &v4addr);
#endif
      
#if UV_VERSION_MAJOR == 0                         
	status = uv_udp_bind(&udp_servers[loc_idx], v4addr,0);
#else    
	status = uv_udp_bind(&udp_servers[loc_idx], (struct sockaddr*)&v4addr, UV_UDP_REUSEADDR);
	      
#endif
	if(status < 0) 
	{
		LERR( "capture: bind error");
	        return 2;
	}

	udp_servers[loc_idx].data = (void *) &loc_idx;

	status = uv_udp_recv_start(&udp_servers[loc_idx], on_alloc, on_recv);

	return 0;
}

int load_module_xml_config() {

	char module_config_name[500];
	xml_node *next;
	int i = 0;

	snprintf(module_config_name, 500, "%s/%s.xml", global_config_path, module_name);

	if ((module_xml_config = xml_parse(module_config_name)) == NULL) {
		LERR("Unable to open configuration file: %s", module_config_name);
		return -1;
	}

	/* check if this module is our */
	next = xml_get("module", module_xml_config, 1);

	if (next == NULL) {
		LERR("wrong config for module: %s", module_name);
		return -2;
	}

	for (i = 0; next->attr[i]; i++) {
			if (!strncmp(next->attr[i], "name", 4)) {
				if (strncmp(next->attr[i + 1], module_name, strlen(module_name))) {
					return -3;
				}
			}
			else if (!strncmp(next->attr[i], "serial", 6)) {
				module_serial = atol(next->attr[i + 1]);
			}
			else if (!strncmp(next->attr[i], "description", 11)) {
				module_description = next->attr[i + 1];
			}
	}

	return 1;
}

void free_module_xml_config() {

	/* now we are free */
	if(module_xml_config) xml_free(module_xml_config);
}

/* modules external API */

static uint64_t serial_module(void)
{
	 return module_serial;
}

static int load_module(xml_node *config) {

	xml_node *params, *profile=NULL, *settings;
	char *key, *value = NULL;
	unsigned int i = 0;
	//char module_api_name[256];
	char loadplan[1024];
	FILE* cfg_stream;

	LNOTICE("Loaded %s", module_name);

	load_module_xml_config();

	/* READ CONFIG */
	profile = module_xml_config;

	/* reset profile */
	profile_size = 0;

	while (profile) {

		profile = xml_get("profile", profile, 1);

		memset(&profile_socket[i], 0, sizeof(profile_socket_t));

		if (profile == NULL)
			break;

		if (!profile->attr[4] || strncmp(profile->attr[4], "enable", 6)) {
			goto nextprofile;
		}

		/* if not equals "true" */
		if (!profile->attr[5] || strncmp(profile->attr[5], "true", 4)) {
			goto nextprofile;
		}

		/* set values */
		profile_socket[profile_size].name = strdup(profile->attr[1]);
		profile_socket[profile_size].description = strdup(profile->attr[3]);
		profile_socket[profile_size].serial = atoi(profile->attr[7]);
		profile_socket[profile_size].protocol = PROTO_SIP; //we extract SIP and send as SIP packet
		profile_socket[profile_size].port = TZSP_PORT;
		profile_socket[profile_size].host = TZSP_HOST;
		
		/* SETTINGS */
		settings = xml_get("settings", profile, 1);

		if (settings != NULL) {

			params = settings;

			while (params) {

				params = xml_get("param", params, 1);
				if (params == NULL)
					break;

				if (params->attr[0] != NULL) {

					/* bad parser */
					if (strncmp(params->attr[0], "name", 4)) {
						LERR("bad keys in the config");
						goto nextparam;
					}

					key = params->attr[1];

					if (params->attr[2] && params->attr[3] && !strncmp(params->attr[2], "value", 5)) {
						value = params->attr[3];
					} else {
						value = params->child->value;
					}

					if (key == NULL || value == NULL) {
						LERR("bad values in the config");
						goto nextparam;

					}

					if (!strncmp(key, "host", 4))
						profile_socket[profile_size].host = strdup(value);
					else if (!strncmp(key, "port", 4))
						profile_socket[profile_size].port = strdup(value);
					else if (!strncmp(key, "protocol-type", 13))
						profile_socket[profile_size].protocol = atoi(value);						
					else if (!strncmp(key, "capture-plan", 12))
						profile_socket[profile_size].capture_plan = strdup(value);
				}

				nextparam: params = params->next;

			}
		}
		
		profile_size++;

		nextprofile: profile = profile->next;
	}

	/* free */		
		
	free_module_xml_config();

#if UV_VERSION_MAJOR == 0
    loop = uv_loop_new();
#else               
    loop = malloc(sizeof *loop);
    uv_loop_init(loop);
#endif

	for (i = 0; i < profile_size; i++) {

		if(profile_socket[i].capture_plan != NULL)
		{

			snprintf(loadplan, sizeof(loadplan), "%s/%s", global_capture_plan_path, profile_socket[i].capture_plan);
			cfg_stream=fopen (loadplan, "r");

			if (cfg_stream==0){
			   fprintf(stderr, "ERROR: loading config file(%s): %s\n", loadplan, strerror(errno));
			}

			yyin=cfg_stream;
			if ((yyparse()!=0)||(cfg_errors)){
			          fprintf(stderr, "ERROR: bad config file (%d errors)\n", cfg_errors);
			}

			profile_socket[i].action = main_ct.idx;
		}

		// start thread
		if (init_socket(i)) {
			LERR("couldn't init tzsp");
			return -1;
		}

		//pthread_create(&call_thread, NULL, proto_collect, arg);

	}
	
	uv_async_init(loop, &async_handle, _async_callback);
	uv_thread_create(&runthread, _run_uv_loop, loop);

	return 0;
}

static int unload_module(void) {
	unsigned int i = 0;

	LNOTICE("unloaded module %s", module_name);

	for (i = 0; i < profile_size; i++) {

		close_socket(i);
		free_profile(i);
	}
	
	                 
#if UV_VERSION_MAJOR == 0
	uv_async_send(&async_handle);
	uv_loop_delete(loop);
#else

	if (uv_loop_alive(loop)) {
        	uv_async_send(&async_handle);
	}
   
	uv_stop(loop);
	uv_loop_close(loop);
	free(loop);
#endif
	/* Close socket */
	return 0;
}

static int free_profile(unsigned int idx) {

	/*free profile chars **/
	if (profile_socket[idx].name)	 free(profile_socket[idx].name);
	if (profile_socket[idx].description) free(profile_socket[idx].description);
	if (profile_socket[idx].device) free(profile_socket[idx].device);
	if (profile_socket[idx].host) free(profile_socket[idx].host);
	if (profile_socket[idx].port) free(profile_socket[idx].port);
	if (profile_socket[idx].capture_plan) free(profile_socket[idx].capture_plan);
	return 1;
}

static int description(char *descr) {
	LNOTICE("Loaded description of %s", module_name);
	descr = module_description;
	return 1;
}

static int statistic(char *buf, size_t len) {

	int ret = 0;

	ret += snprintf(buf+ret, len-ret, "Total received: [%" PRId64 "]\r\n", stats.recieved_packets_total);
	ret += snprintf(buf+ret, len-ret, "TCP received: [%" PRId64 "]\r\n", stats.recieved_tcp_packets);
	ret += snprintf(buf+ret, len-ret, "UDP received: [%" PRId64 "]\r\n", stats.recieved_udp_packets);
	ret += snprintf(buf+ret, len-ret, "SCTP received: [%" PRId64 "]\r\n", stats.recieved_sctp_packets);
	ret += snprintf(buf+ret, len-ret, "Total sent: [%" PRId64 "]\r\n", stats.send_packets);


	return 1;
}
