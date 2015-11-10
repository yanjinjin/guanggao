#include "main.h"

struct list_head httpc_list;
struct ipq_msg ipqm;

unsigned short in_cksum(unsigned short *addr, int len)    /* function is from ping.c */
{
    register int nleft = len;
    register u_short *w = addr;
    register int sum = 0;
    u_short answer =0;
 
    while (nleft > 1)
        {
        sum += *w++;
        nleft -= 2;
        }
    if (nleft == 1)
        {      
        *(u_char *)(&answer) = *(u_char *)w;
        sum += answer;
        }
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    answer = ~sum;
    return(answer);
}

unsigned short ip_chsum(struct iphdr *iph)
{
	unsigned short check;
	iph->check=0;
	check=in_cksum((unsigned short *)iph, sizeof(struct iphdr));
	return check;
}

unsigned short tcp_chsum(struct iphdr *iph , struct tcphdr *tcp ,int tcp_len)
{
	char check_buf[BUFSIZE]={0};
	unsigned short check;
	
    struct pseudo_header
    {
        unsigned int source_address;
        unsigned int dest_address;
        unsigned char placeholder;
        unsigned char protocol;
        unsigned short tcp_length;
    } pseudo;
	
	tcp->check=0;

    // set the pseudo header fields 
    pseudo.source_address = iph->saddr;
    pseudo.dest_address = iph->daddr;
    pseudo.placeholder = 0;
    pseudo.protocol = IPPROTO_TCP;
    pseudo.tcp_length = htons(tcp_len);
	memcpy(check_buf,&pseudo,sizeof(struct pseudo_header));
	memcpy(check_buf+sizeof(struct pseudo_header),tcp,tcp_len);
    check = in_cksum((unsigned short *)&check_buf, sizeof(struct pseudo_header)+tcp_len);
	
	return check;
	
}

int send_to_knl(struct _skb *skb , int len , unsigned char* buf , char type)
{
	thread_lock();	
	ipq_set_verdict(ipqm.h, skb->m->packet_id,
			 type, len, buf);
	thread_unlock();	
	return 0;
}

int send_one_package_accept(struct _skb *skb)
{
	send_to_knl(skb , skb->m->data_len , skb->m->payload ,NF_ACCEPT);
	return 0;

}

int send_one_package_drop(struct _skb *skb)
{
	send_to_knl(skb , skb->m->data_len , skb->m->payload ,NF_DROP);
	return 0;

}

int insert_code(struct _skb *skb)
{
    char* body;
    char buffer[BUFSIZE];
	int len=0;
	
    if(!skb->http_head)
		return -1;

    if(!skb->http_data)
		return -1;
    body=strstr(skb->http_data , "<body>");
	if(!body)
		return -1;
    body=body+6;
	
	len=strlen(body);
	memcpy(buffer , skb->http_head , skb->http_len-len);
	memcpy(buffer+(skb->http_len-len) , "hello" , 5);
	memcpy(buffer+(skb->http_len-len+5) , body , len);
	
  	memcpy(skb->http_head , buffer , skb->http_len+5);
	body=strstr(skb->http_head , skb->hhdr.content_length);
	if(!body)
		return -1;
	memcpy(body , "2952" , 4);
	debug_log("````````````%s\n" , skb->http_head);
    skb->iph->tot_len=htons(skb->http_len+5);
    skb->iph->check=ip_chsum(skb->iph);

    skb->tcp->check=tcp_chsum(skb->iph , skb->tcp , skb->tcp_len+5);
	skb->m->data_len=skb->m->data_len+5;
    return 0;
}

int timeout_content_chunked(struct request_conntrack *reqc)
{
	struct response_conntrack *resc_cursor , *resc_tmp ;
	list_for_each_entry_safe(resc_cursor, resc_tmp, &(reqc->response_conntrack_list), list)
	{
		send_one_package_accept(resc_cursor->skb);
		thread_lock();
		la_list_del(&resc_cursor->list);
		free_page(resc_cursor->skb);
		free_page(resc_cursor);
		resc_cursor=NULL;
		reqc->response_conntrack_num--;
		ipqm.current_skb_num--;
		thread_unlock();
	}
}

int timeout_content_length(struct request_conntrack *reqc)
{
	struct response_conntrack *resc_cursor , *resc_tmp ;

	
	if(reqc->curr_content_length == reqc->content_length)
	{	
		list_for_each_entry_safe(resc_cursor, resc_tmp, &(reqc->response_conntrack_list), list)
		{
			if(-1==insert_code(resc_cursor->skb))
			{
				resc_cursor->skb->tcp->seq=htonl(ntohl(resc_cursor->skb->tcp->seq)+5);
			    resc_cursor->skb->tcp->check=tcp_chsum(resc_cursor->skb->iph , resc_cursor->skb->tcp , resc_cursor->skb->tcp_len);
			}
			debug_log("!!!!!!!!!!!!!!!!%s\n" ,  resc_cursor->skb->http_head);
			send_one_package_accept(resc_cursor->skb);
			thread_lock();
	        la_list_del(&resc_cursor->list);
	        free_page(resc_cursor->skb);
	        free_page(resc_cursor);
	        resc_cursor=NULL;
			reqc->response_conntrack_num--;
	        ipqm.current_skb_num--;
	        thread_unlock();
		}
		
		return 1;
	}
	return 0;
}

void timeout(void* arg)
{
	struct http_conntrack *httpc_cursor , *httpc_tmp;
	struct request_conntrack *reqc_cursor , *reqc_tmp;
	
	while(1)
	{
		if(ipqm.current_skb_num <=0)
		{
			mnanosleep(1000);
			continue;
		}
		mnanosleep(1000);
		list_for_each_entry_safe(httpc_cursor, httpc_tmp, &httpc_list, list)
		{
			list_for_each_entry_safe(reqc_cursor, reqc_tmp, &(httpc_cursor->request_conntrack_list), list)
			{	
				if( reqc_cursor->skb->hhdr.res_type==HTTP_RESPONSE_TYPE_CONTENTLENGTH)
				{						
					timeout_content_length(reqc_cursor);
				}
				else
				{
					timeout_content_chunked(reqc_cursor);
				}
			}
		}	
	}
}

struct http_conntrack* find_http_conntrack(struct _skb *skb)
{
	struct http_conntrack *cursor , *tmp;
	list_for_each_entry_safe(cursor, tmp, &httpc_list, list)
	{
		if(!strcmp(skb->hhdr.host , cursor->host) ||
			skb->iph->saddr == cursor->ip ||
			skb->iph->daddr == cursor->ip)
		{
			return cursor;
		}
	}	
	return NULL;
}

struct request_conntrack* find_request_conntrack_by_uri(struct http_conntrack *httpc ,struct _skb *skb)
{
	struct request_conntrack *cursor , *tmp;
	list_for_each_entry_safe(cursor, tmp, &(httpc->request_conntrack_list), list)
	{
		if(skb->iph->daddr == cursor->skb->iph->daddr &&
			!strcmp(skb->hhdr.uri , cursor->skb->hhdr.uri))
		{
			return cursor;
		}
	}	
	return NULL;
}

struct request_conntrack* find_request_conntrack_by_ack(struct http_conntrack *httpc ,struct _skb *skb)
{
	struct request_conntrack *cursor , *tmp;
	int i=0;
	
	list_for_each_entry_safe(cursor, tmp, &(httpc->request_conntrack_list), list)
	{
		/*debug_log("%d---%lu--%lu---%lu---%lu---%s\n%lu--%lu---%lu---%lu--%d---%s\n" ,
						i++,ntohl(skb->iph->saddr),ntohl(skb->iph->daddr),
						ntohl(skb->tcp->seq),ntohl(skb->tcp->ack_seq),
						skb->http_head,
		
						ntohl(cursor->skb->iph->saddr),ntohl(cursor->skb->iph->daddr) ,
						ntohl(cursor->skb->tcp->seq),ntohl(cursor->skb->tcp->ack_seq) ,
						cursor->skb->http_len, cursor->skb->http_head);*/
		
		if(skb->iph->saddr == cursor->skb->iph->daddr &&
			ntohl(skb->tcp->ack_seq) == ntohl(cursor->skb->tcp->seq)+cursor->skb->http_len)
		{
			return cursor;
		}
	}	
	return NULL;
}


int decode_http(struct _skb *skb)
{
	char **toks = NULL;
	int num_toks=0,tmp_num_toks=0;
	int i = 0;
	char **opts;
	int num_opts=0;
	char req_post[][16]={"5" ,"POST " };
	char req_get[][16]={"4" ,"GET " };
	char res[][16]={"7" ,"HTTP/1."};
	char http_head_end[][16]={"4" ,"\r\n\r\n"};
	
	skb->http_len=skb->ip_len-skb->iph_len-skb->tcph_len;
	if(skb->http_len<=0)
		return -1;
	
	skb->http_head=(char*)(skb->m->payload +skb->iph_len+skb->tcph_len);
	if(!skb->http_head)
		return -1;

	//////////////http_head_start///////////
	if(!memcmp(skb->http_head,req_post[1],atoi(req_post[0])))
	{
		skb->hhdr.http_type=HTTP_TYPE_REQUEST_POST;
	}
	else if(!memcmp(skb->http_head,req_get[1],atoi(req_get[0])))
	{
		skb->hhdr.http_type=HTTP_TYPE_REQUEST_GET;
	}
	else if(!memcmp(skb->http_head,res[1],atoi(res[0])))
	{
			
		skb->hhdr.http_type=HTTP_TYPE_RESPONSE;
	}
	else 
	{
		skb->hhdr.http_type=HTTP_TYPE_OTHER;
		return 0;
	}
	
	toks = mSplit(skb->http_head, "\r\n", MAX_PATTERN_NUM, &num_toks,'\\');

	tmp_num_toks=num_toks;
	num_toks--;
	while(num_toks)
	{ 
		if(i==0)
		{
			opts = mSplit(toks[i], " ", 3, &num_opts,'\\');
			while(isspace((int)*opts[0])) opts[0]++;
			if(skb->hhdr.http_type==HTTP_TYPE_RESPONSE)
			{
				strncpy(skb->hhdr.error_code, opts[1] ,COMM_MAX_LEN);
			}
			else if(skb->hhdr.http_type==HTTP_TYPE_REQUEST_GET||
				skb->hhdr.http_type==HTTP_TYPE_REQUEST_POST)
			{
				strncpy(skb->hhdr.uri , opts[1] , COMM_MAX_LEN);
			}
		}
		else
		{
			opts = mSplit(toks[i], ": ", 2, &num_opts,'\\');
			while(isspace((int)*opts[0])) opts[0]++;
			
			if(!strcasecmp(opts[0], "host"))
			{
				strncpy(skb->hhdr.host , opts[1] ,COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "accept-encoding"))
			{
				strncpy(skb->hhdr.accept_encoding , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "accept"))
			{
				strncpy(skb->hhdr.accept , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "accept-charset"))
			{
				strncpy(skb->hhdr.accept_charset , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "accept-language"))
			{
				strncpy(skb->hhdr.accept_language , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "authorization"))
			{
				strncpy(skb->hhdr.authorization , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "cache-control"))
			{
				strncpy(skb->hhdr.cache_control , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "connection"))
			{
				strncpy(skb->hhdr.connection , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "content-encoding"))
			{
				strncpy(skb->hhdr.content_encoding , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "content-language"))
			{
				strncpy(skb->hhdr.content_language , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "content-length"))
			{
				strncpy(skb->hhdr.content_length , opts[1],COMM_MAX_LEN);
				skb->hhdr.res_type=HTTP_RESPONSE_TYPE_CONTENTLENGTH;
			}
			else if(!strcasecmp(opts[0], "content-type"))
			{
				strncpy(skb->hhdr.content_type , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "content-range"))
			{
				strncpy(skb->hhdr.content_range , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "connection"))
			{
				strncpy(skb->hhdr.connection , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "user-agent"))
			{
				strncpy(skb->hhdr.user_agent , opts[1],COMM_MAX_LEN);
			}
			else if(!strcasecmp(opts[0], "transfer-encoding"))
			{
				strncpy(skb->hhdr.transfer_encoding , opts[1],COMM_MAX_LEN);
				if(!strcmp(skb->hhdr.transfer_encoding , "chunked"))
				{
					skb->hhdr.res_type=HTTP_RESPONSE_TYPE_CHUNKED;
					
				}
			}	
		}
		mSplitFree(&opts ,num_opts);
		--num_toks;
		i++;
	}
	mSplitFree(&toks ,tmp_num_toks);

	//////////////http_head_end///////////
	skb->http_data=strstr(skb->http_head, http_head_end[1]);
	if(!skb->http_data)
	{
		return -1;
	}
	
	skb->httph_len=strlen(skb->http_head)-strlen(skb->http_data);
	
	return 0;
}

int decode_tcp(struct _skb *skb)
{
	skb->tcp = (struct tcphdr *)(skb->m->payload +skb->iph_len);
	if(!skb->tcp)
		return -1;
	skb->tcph_len = 4 * skb->tcp->doff;
	skb->tcp_len = skb->ip_len-skb->iph_len;
	
	if(-1==decode_http(skb))
		return -1;

	return 0;

}

int decode_ip(struct _skb *skb)
{
	
	skb->m = ipq_get_packet(skb->buf);
	skb->iph = (struct iphdr *)(skb->m->payload);
	if(!skb->iph)
		return -1;
	if(skb->iph->ihl < 5 || skb->iph->version != 4)
		return -1;
	skb->ip_len = ntohs(skb->iph->tot_len);
	if(skb->ip_len != skb->m->data_len)
		return -1;
	skb->iph_len=4 * skb->iph->ihl;
	if (skb->ip_len < skb->iph_len)
		return -1;
	
	
	if(skb->iph->protocol!=IPPROTO_TCP)
		return -1;
	
	if(-1==decode_tcp(skb))
		return -1;
	
	
	return 0;
}

void decode(void* arg)
{
	struct http_conntrack *httpc;
	struct request_conntrack *reqc;
	struct response_conntrack *resc;
	struct _skb* skb;
	
	while(1)
	{
		skb=(struct _skb*)new_page(sizeof(struct  _skb));
		if(!skb)
		{
			continue;
		}
		get_queue(skb->buf);
		
		if(-1==decode_ip(skb))
		{
			send_one_package_accept(skb);
			free_page(skb);
			continue;
		}
		
		/*char sip[COMM_MAX_LEN],dip[COMM_MAX_LEN];
		ip2addr(sip , ntohl(skb.iph->saddr));
		ip2addr(dip , ntohl(skb.iph->daddr));
		debug_log("%s-->%s seq:%lu , ack_seq:%lu \n" , 
			sip ,dip,
			ntohl(skb.tcp->seq) , ntohl(skb.tcp->ack_seq));
		*/
		if(skb->http_len <=1 || !skb->http_head )
		{
			send_one_package_accept(skb);
			free_page(skb);
			continue;
		}
		httpc=find_http_conntrack(skb);
		if(!httpc)
		{
			httpc=(struct http_conntrack*)new_page(sizeof(struct  http_conntrack));
			if(!httpc)
			{
				send_one_package_accept(skb);
				free_page(skb);
				continue;
			}
			thread_lock();	
			httpc->ip = skb->iph->daddr;
			strcpy(httpc->host , skb->hhdr.host);
			INIT_LIST_HEAD(&(httpc->request_conntrack_list));
			la_list_add_tail(&(httpc->list), &(httpc_list));
			thread_unlock();
			
		}
		
		switch (skb->hhdr.http_type)
		{
			case HTTP_TYPE_REQUEST_GET:
			{
				if(0!=strcmp(skb->hhdr.uri , "/"))
				{
					send_one_package_accept(skb);
					free_page(skb);
					continue;

				}
				reqc=find_request_conntrack_by_uri(httpc ,skb);
				if(!reqc)
				{
					reqc=(struct request_conntrack*)new_page(sizeof(struct  request_conntrack));
					if(!reqc)
					{
						send_one_package_accept(skb);
						free_page(skb);
						continue;
					}
					thread_lock();		
					reqc->skb=skb;
					INIT_LIST_HEAD(&(reqc->response_conntrack_list));
					la_list_add_tail(&(reqc->list),&(httpc->request_conntrack_list));
					httpc->request_conntrack_num++;		
					thread_unlock();
					send_one_package_accept(skb);
				}
				else
				{
					free_page(reqc->skb);
					reqc->skb=skb;
					send_one_package_accept(skb);
				}
				break;
			}
			case HTTP_TYPE_REQUEST_POST:
				send_one_package_accept(skb);
				free_page(skb);
				break;
			case HTTP_TYPE_RESPONSE:
			case HTTP_TYPE_OTHER:
			default:
			{
				reqc=find_request_conntrack_by_ack(httpc ,skb);
				if(!reqc)
				{
					send_one_package_accept(skb);
					free_page(skb);
				}
				else
				{
					resc=(struct response_conntrack*)new_page(sizeof(struct  response_conntrack));
					if(!resc)
					{
						send_one_package_accept(skb);
						free_page(skb);
						continue;
					}
					thread_lock();	
					resc->skb=skb;				
					reqc->response_conntrack_num++;
					ipqm.current_skb_num++;
					if(resc->skb->hhdr.res_type!=HTTP_RESPONSE_TYPE_OTHER)
					{
						reqc->skb->hhdr.res_type = resc->skb->hhdr.res_type;
						if(reqc->skb->hhdr.res_type == HTTP_RESPONSE_TYPE_CONTENTLENGTH)
						{
							strcpy(reqc->skb->hhdr.content_length , resc->skb->hhdr.content_length);
							reqc->content_length =atoi(resc->skb->hhdr.content_length) + 
								resc->skb->httph_len+strlen("\r\n\r\n");
						}
					}
					reqc->curr_content_length=reqc->curr_content_length+resc->skb->http_len;
					la_list_add_tail(&(resc->list),&(reqc->response_conntrack_list));
					thread_unlock();
				}
				break;			
			}			
		}
	}
}

static void die(struct ipq_handle *h) 
{
    ipq_perror("passer");
    ipq_destroy_handle(h);
    exit(1);
}

int main(int argc, char **argv) 
{
	init_mpool(1*1024*1024);//256M
	
	INIT_LIST_HEAD(&httpc_list);
	init_queue();
	init_thpool(3);
	thpool_add_job(decode , NULL);
	thpool_add_job(timeout , NULL);
	//thpool_add_job(remote , NULL);

	memset(&ipqm , '\0' , sizeof(struct ipq_msg));
	
    ipqm.h = ipq_create_handle(0, NFPROTO_IPV4);
    if (!ipqm.h)
        die(ipqm.h);
    ipqm.status = ipq_set_mode(ipqm.h, IPQ_COPY_PACKET, BUFSIZE);
    if (ipqm.status < 0)
        die(ipqm.h);
    do{
        ipqm.status = ipq_read(ipqm.h, ipqm.buf, BUFSIZE, 0);
        if (ipqm.status < 0)//Failed to receive netlink message: No buffer space available
        {
			continue;
        }
		
        switch (ipq_message_type(ipqm.buf)) {
            case NLMSG_ERROR:
                debug_log("Received error message %d\n",
                        ipq_get_msgerr(ipqm.buf));
                break;
            case IPQM_PACKET: {
				set_queue(ipqm.buf , BUFSIZE);              
                if (ipqm.status < 0)
                    continue;
                break;
            }
            default:
                debug_log("Unknown message type!\n");
                break;
        }
    } while (1);
    ipq_destroy_handle(ipqm.h);
	fini_thpool();
    return 0;
}
