#include	"config.h"
#include	"includes.h"
#include	"freeradius-client.h"
#include	"pathnames.h"
#include	"message.h"
#include    "list.h"
#include    "libcom.h"
#include     "log.h"
#include     "nlk_ipc.h"

#define RC_CONFIG_FILE "/usr/local/etc/radiusclient/radiusclient.conf"

static char *pname = NULL;
char		*default_realm = NULL;
rc_handle	*rh = NULL;
pthread_mutex_t auth_mutex;
static int pipefd[2];

typedef struct authenticated_cfg_st{
	uint32 ipaddr;
	uint8 mac[6];
    int8 acct_status;   /*0:none; 1:accounting*/
    int8 acct_policy;   /*1:accounting by time; 2:accounting by flow; 3(1&2):accounting by time and flow*/
    uint64 total_seconds;
    uint64 total_flows;
}authenticated_cfg_t;

typedef struct file_config_st{
	uint64 tt_seconds;
	uint64 tt_flows;
	int8 acct_sta;
	int8 acct_poli;
}file_config_t;

file_config_t temp_config;

typedef struct user_info
{
	char name[20];		//字符串
	char pwd[20];		//字符串
	unsigned char mac[6];
	uint32 ipaddr;
}user_info_t;

char temp_mac[6] = {0};
char dev_mac[20] = {0};

int get_dev_mac()
{
	char **array = NULL;
	int num, i = 0;
	if (!uuci_get("network.wan.macaddr", &array, &num)) {
		arr_strcpy(dev_mac, array[0]);
		uuci_get_free(array, num);
	}
	while (dev_mac[i] != '\0') {
		if (dev_mac[i] == ':')
			dev_mac[i] = '-';
		i++;
	}
	return 0;
}

int cal_session_id(char *session_id, int size, user_query_info_t *u)
{
	char raw_str[64] = {0};
	snprintf(raw_str, sizeof(raw_str) - 1, "a%s%s%d123456", u->username, u->password, (int)time(NULL));
	unsigned char md5[17] = {0};
	char md5_str[64] = {0};
	rc_md5_calc(md5, raw_str, strlen(raw_str));
	int i = 0;
	for (i = 0; i < 16; i++) {
		snprintf(&md5_str[i*2], 3, "%02x", md5[i]);
	}
	snprintf(session_id, size - 1, "%s%s", md5_str, md5_str);
	session_id[38] = '\0';
	return 0;
}

int tem = 0;

typedef struct auth_ok_user
{
	struct list_head list;
	user_query_info_t user_info;
	time_t t;
	char acct_session[64];
	authenticated_cfg_t ucfg;
}auth_ok_user_t;

static LIST_HEAD(auth_ok_list);    /*认证通过链表*/

int acct_handler(user_query_info_t *u, int acct_type, char *session_id)
{
	char 		msg[PW_MAX_MSG_SIZE], username_realm[256];
	VALUE_PAIR 	*send = NULL;
	uint32_t		service;
	uint32_t		value_temp;
	int result, ret = -1;
	struct in_addr ip;

	strncpy(username_realm, u->username, sizeof(username_realm));

	if ((strchr(username_realm, '@') == NULL) && default_realm &&
	    (*default_realm != '\0')) {
		strncat(username_realm, "@", sizeof(username_realm)-strlen(username_realm)-1);
		strncat(username_realm, default_realm, sizeof(username_realm)-strlen(username_realm)-1);
	}

	if (rc_avpair_add(rh, &send, PW_USER_NAME, username_realm, -1, 0) == NULL)
		goto error;

	if (rc_avpair_add(rh, &send, PW_NAS_IDENTIFIER, "JFGW-1.0", -1, 0) == NULL)
		goto error;
	
	value_temp = 19;
	if (rc_avpair_add(rh, &send, PW_NAS_PORT_TYPE, &value_temp, 4, 0) == NULL)
		goto error;

	char infoset[64] = {0};
	snprintf(infoset, sizeof(infoset) - 1, "slot=1;subslot=0;port=0;vlanid=%d;", u->vlan);
	if (rc_avpair_add(rh, &send, 87, infoset, -1, 0) == NULL)
		goto error;

	value_temp = acct_type;
	if (rc_avpair_add(rh, &send, PW_ACCT_STATUS_TYPE, &value_temp, 4, 0) == NULL)
		goto error;
	
	value_temp = 3600 * 12 + 100;
	if (rc_avpair_add(rh, &send, PW_ACCT_SESSION_TIME, &value_temp, 4, 0) == NULL)
		goto error;

	ip.s_addr = inet_addr(u->user_ip);
	ip.s_addr = htonl(ip.s_addr);
	if (rc_avpair_add(rh, &send, PW_FRAMED_IP_ADDRESS, &ip.s_addr, 4, 0) == NULL)
		goto error;
	
	value_temp = 1;
	if (rc_avpair_add(rh, &send, PW_FRAMED_PROTOCOL, &value_temp, 4, 0) == NULL)
		goto error;

	
	if (rc_avpair_add(rh, &send, PW_ACCT_SESSION_ID, session_id, -1, 0) == NULL)
		goto error;

	char temp_mac[32] = {0};
	strcpy(temp_mac, u->mac);
	macformat(temp_mac, '-');
	if (rc_avpair_add(rh, &send, PW_CALLING_STATION_ID, temp_mac, -1, 0) == NULL)
		goto error;

	char temp_called_station_id[32] = {0};
	snprintf(temp_called_station_id, 31, "%s:jfwx608", dev_mac);
	if (rc_avpair_add(rh, &send, PW_CALLED_STATION_ID, temp_called_station_id, -1, 0) == NULL)
		goto error;

	char wan_ip[20] = {0};
	int rlen = 0;
	if ((shell_printf("ifstatus wan | grep \"address\" | grep -oE '[0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}'",
					wan_ip, sizeof(wan_ip))) > 0) {
		wan_ip[rlen - 1] = '\0';
	}
	
	ip.s_addr = inet_addr(wan_ip);
	ip.s_addr = htonl(ip.s_addr);
	if (rc_avpair_add(rh, &send, PW_NAS_IP_ADDRESS, &ip.s_addr, 4, 0) == NULL)
		goto error;

	service = PW_FRAMED;
	if (rc_avpair_add(rh, &send, PW_SERVICE_TYPE, &service, -1, 0) == NULL)
		goto error;


	result = rc_acct(rh, 0, send);
	if (result == OK_RC) {
		printf("\"%s\" RADIUS acct OK \n", u->username);
		ret = 0;
	} else {
		printf("\"%s\" RADIUS acct failure (RC=%i)  msg is %s\n", u->username, result, msg);
		ret = 1;
	}

error:
	if (send)
		rc_avpair_free(send);
	
	return ret;		
}


int32 auth_handler(const int32 cmd, void *ibuf, int32 ilen, void *obuf, int32 *olen)
{
	char 		msg[PW_MAX_MSG_SIZE], username_realm[256];
	VALUE_PAIR 	*send = NULL, *received = NULL;
	uint32_t		service;
	int result, ret = -1, value_temp;
	struct in_addr ip;
	user_query_info_t *u = (user_query_info_t *)ibuf;
	
	auth_ok_user_t *p = NULL;
	/*已经认证过，直接返回*/
	pthread_mutex_lock(&auth_mutex);
	list_for_each_entry(p, &auth_ok_list, list) {
		if (strcmp(p->user_info.mac, u->mac) == 0) {
			ret = 0;
			*olen = 0;
			pthread_mutex_unlock(&auth_mutex);
			goto error;
		}
	}
	pthread_mutex_unlock(&auth_mutex);
	//Fill in User-Name
	strncpy(username_realm, u->username, sizeof(username_realm));
		printf("uname %s pwd %s\n", u->username, u->password);
	/* Append default realm */
	if ((strchr(username_realm, '@') == NULL) && default_realm &&
	    (*default_realm != '\0')) {
		strncat(username_realm, "@", sizeof(username_realm)-strlen(username_realm)-1);
		strncat(username_realm, default_realm, sizeof(username_realm)-strlen(username_realm)-1);
	}

	if (rc_avpair_add(rh, &send, PW_USER_NAME, username_realm, -1, 0) == NULL)
		goto error;

	// Fill in User-Password
	
	char chap_passwd[40] = "a";
	strcat(chap_passwd, u->password);
	strcat(chap_passwd, "1234567890123456");
	char md5[20] = {0};
	rc_md5_calc(md5, chap_passwd, strlen(chap_passwd));

	char temp[20] = "a";
	memcpy(&temp[1], md5, 16);

	if (rc_avpair_add(rh, &send, PW_CHAP_PASSWORD, temp, 17, 0) == NULL)
		goto error;

	if (rc_avpair_add(rh, &send, PW_CHAP_CHALLENGE, "1234567890123456", -1, 0) == NULL)
		goto error;

	if (rc_avpair_add(rh, &send, PW_NAS_IDENTIFIER, "JFGW-1.0", -1, 0) == NULL)
		goto error;

	value_temp = 19;
	if (rc_avpair_add(rh, &send, PW_NAS_PORT_TYPE, &value_temp, 4, 0) == NULL)
		goto error;

	char infoset[64] = {0};
	snprintf(infoset, sizeof(infoset) - 1, "slot=1;subslot=0;port=0;vlanid=%d;", u->vlan);
	if (rc_avpair_add(rh, &send, 87, infoset, -1, 0) == NULL)
		goto error;
	
	
	ip.s_addr = inet_addr(u->user_ip);
	ip.s_addr = htonl(ip.s_addr);
	if (rc_avpair_add(rh, &send, PW_FRAMED_IP_ADDRESS, &ip.s_addr, 4, 0) == NULL)
		goto error;
	
	value_temp = 1;
	if (rc_avpair_add(rh, &send, PW_FRAMED_PROTOCOL, &value_temp, 4, 0) == NULL)
		goto error;

	char temp_mac[32] = {0};
	strcpy(temp_mac, u->mac);
	macformat(temp_mac, '-');
	if (rc_avpair_add(rh, &send, PW_CALLING_STATION_ID, temp_mac, -1, 0) == NULL)
		goto error;
	
	char temp_called_station_id[32] = {0};
	snprintf(temp_called_station_id, 31, "%s:jfwx608", dev_mac);
	if (rc_avpair_add(rh, &send, PW_CALLED_STATION_ID, temp_called_station_id, -1, 0) == NULL)
		goto error;
	
	char wan_ip[20] = {0};
	int rlen = 0;
	if ((shell_printf("ifstatus wan | grep \"address\" | grep -oE '[0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}.[0-9]{1,3}'",
					wan_ip, sizeof(wan_ip))) > 0) {
		wan_ip[rlen - 1] = '\0';
	}
	ip.s_addr = inet_addr(wan_ip);
	ip.s_addr = htonl(ip.s_addr);
	if (rc_avpair_add(rh, &send, PW_NAS_IP_ADDRESS, &ip.s_addr, 4, 0) == NULL)
		goto error;

	//Fill in Service-Type
	service = PW_FRAMED;
	if (rc_avpair_add(rh, &send, PW_SERVICE_TYPE, &service, -1, 0) == NULL)
		goto error;

	result = rc_auth(rh, 0, send, &received, msg);
	printf("send auth\n");
	printf("mac:%s\n", u->mac);
	if (result == OK_RC) {
		printf("\"%s\" RADIUS Authentication OK \n", u->username);
		
		char session_id[64] = {0};
		cal_session_id(session_id, sizeof(session_id), u);
		if (acct_handler(u, PW_STATUS_START, session_id))
			goto error;
		auth_ok_user_t *auth_ok_u = malloc(sizeof(auth_ok_user_t));
		memcpy(&auth_ok_u->user_info, u, sizeof(user_query_info_t));
		auth_ok_u->t = time(NULL);
		
		authenticated_cfg_t to_as;
		memset(&auth_ok_u->ucfg, 0, sizeof(authenticated_cfg_t));
		str2mac(u->mac, auth_ok_u->ucfg.mac);
		auth_ok_u->ucfg.total_seconds = temp_config.tt_seconds;
		auth_ok_u->ucfg.acct_status = temp_config.acct_sta;
		auth_ok_u->ucfg.acct_policy = temp_config.acct_poli;
		auth_ok_u->ucfg.total_flows = temp_config.tt_flows;
		auth_ok_u->ucfg.ipaddr = inet_addr(u->user_ip);
		
		char *rcv_buf = NULL;
		printf("send to as\n");
		if (msg_send_syn( MSG_CMD_AS_AUTHENTICATED_ADD,&auth_ok_u->ucfg,
					sizeof(auth_ok_u->ucfg), &rcv_buf, &rlen) != 0) {
			printf("MSG_CMD_AS_AUTHENTICATED_ADD err\n ");
			if (auth_ok_u)
					free(auth_ok_u);
			goto error;
		}
		strncpy(auth_ok_u->acct_session, session_id, sizeof(auth_ok_u->acct_session) - 1);
		pthread_mutex_lock(&auth_mutex);
		list_add(&auth_ok_u->list, &auth_ok_list);
		pthread_mutex_unlock(&auth_mutex);
		printf("add to list\n");
		AAA_LOG("%02x %02x %02x %02x %02x %02x, time:%d\n", auth_ok_u->ucfg.mac[0], auth_ok_u->ucfg.mac[1],
				auth_ok_u->ucfg.mac[2], auth_ok_u->ucfg.mac[3], auth_ok_u->ucfg.mac[4], 
				auth_ok_u->ucfg.mac[5],auth_ok_u->t);
		if (rcv_buf)
			free_rcv_buf(rcv_buf);
		
		*olen = 0;
		ret = 0;
	}
	else {
		printf("\"%s\" RADIUS Authentication failure (RC=%i)  msg is %s\n", u->username, result, msg);
		ret = ERR_CODE_AUTHFAIL;
	}
error:
	if (received)
		rc_avpair_free(received);
	if (send)
		rc_avpair_free(send);
	if (ret)
		*olen = 0;
	return ret;
	
}

int32 user_timeout(const int32 cmd, void *ibuf, int32 ilen, void *obuf, int32 *olen)
{
	if (ilen != 6)
		return -1;
	auth_ok_user_t *p;
	auth_ok_user_t *n;
	int matched = 0;
	printf("have recv timeout %d\n", ilen);
	pthread_mutex_lock(&auth_mutex);
	list_for_each_entry_safe(p, n, &auth_ok_list, list) {
		if (memcmp(p->ucfg.mac, ibuf, sizeof(p->ucfg.mac)) == 0) {
			list_del(&p->list);
			matched = 1;
//			free(p);
			break;
		}
	}
	pthread_mutex_unlock(&auth_mutex);
	if (matched) {
		acct_handler(&p->user_info, PW_STATUS_STOP, p->acct_session);
		free(p);
	}
	*olen = 0;
	return 0;
}

void sig_hander( int sig )  
{  

	int save_errno = errno;
	int msg = sig;
	send(pipefd[1], (char *)&msg, 1, 0);
	errno = save_errno;
} 

int delete_mac() 
{
	char *rcv_buf = NULL;
	int rlen = 0;
	auth_ok_user_t *u = NULL;
	u = list_first_entry(&auth_ok_list, auth_ok_user_t, list);
	
	if (&u->list != &auth_ok_list) {
		printf("send delete to as\n");
		if (msg_send_syn( MSG_CMD_AS_AUTHENTICATED_DELETE, u->ucfg.mac, sizeof(u->ucfg.mac), &rcv_buf, &rlen) != 0) {
			printf("MSG_CMD_AS_AUTHENTICATED_DELETE err\n ");
			
		} else {
			pthread_mutex_lock(&auth_mutex);
			list_del(&u->list);
			pthread_mutex_unlock(&auth_mutex);
		}
	} 
//		else {
//		char temp_mac[6] = {0};
//		str2mac("78:a3:51:32:39:87", temp_mac);
//		if (msg_send_syn( MSG_CMD_AS_AUTHENTICATED_DELETE, temp_mac, sizeof(temp_mac), &rcv_buf, &rlen) != 0)
//			printf("MSG_CMD_AS_AUTHENTICATED_DELETE err\n ");
//	}
	printf("free before\n");
	if (rcv_buf)
		free_rcv_buf(rcv_buf);
	printf("free after\n");

}

void config_update()
{
	char **array = NULL;
	char *res;
	int num = 0;
	if (!uuci_get("acct_config.acct_config.acct_policy", &array, &num)) {
		temp_config.acct_poli = atoi(array[0]);
		uuci_get_free(array, num);
	}
	if (!uuci_get("acct_config.acct_config.acct_status", &array, &num)) {
		temp_config.acct_sta = atoi(array[0]);
		uuci_get_free(array, num);
	}
	if (!uuci_get("acct_config.acct_config.total_seconds", &array, &num)) {
		temp_config.tt_seconds = simple_strtoull(array[0], &res, 10);
		uuci_get_free(array, num);
	}
	if (!uuci_get("acct_config.acct_config.total_flows", &array, &num)) {
		temp_config.tt_flows = simple_strtoull(array[0], &res, 10);
		uuci_get_free(array, num);
	}
		
}

int radius_sig_init()
{
	sigset_t sig;

	sigemptyset(&sig);
	sigaddset(&sig, SIGABRT);
	sigaddset(&sig, SIGPIPE);
	sigaddset(&sig, SIGQUIT);
	sigaddset(&sig, SIGUSR1);
	sigaddset(&sig, SIGUSR2);
	sigaddset(&sig, SIGHUP);
	sigaddset(&sig, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &sig, NULL);
	
	signal(SIGINT, sig_hander);
	signal(SIGTERM, sig_hander);
//	signal(SIGBUS, sig_hander);
//	signal(SIGFPE, sig_hander);
//	signal(SIGSEGV, sig_hander);
	return 0;

}


int main(int argc, char **argv)
{

	int i = 0, ret = -1;
	pname = (pname = strrchr(argv[0],'/'))?pname+1:argv[0];

	rc_openlog(pname);
	if ((rh = rc_read_config(RC_CONFIG_FILE)) == NULL)
		return ERROR_RC;

	if (rc_read_dictionary(rh, rc_conf_str(rh, "dictionary")) != 0)
		return ERROR_RC;
	default_realm = rc_conf_str(rh, "default_realm");
	get_dev_mac();
	config_update();
	pthread_mutex_init(&auth_mutex, NULL);
	
	ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
	
	msg_init(MODULE_RADIUS);
	msg_cmd_register(MSG_CMD_RADIUS_USER_AUTH, auth_handler);
	msg_cmd_register(MSG_CMD_RADIUS_AUTH_TIMEOUT, user_timeout);
	msg_dst_module_register_netlink(MODULE_AS);
	
	radius_sig_init();
	
//	timer_list_init(1, sig_hander);
	struct timeval tv;
	fd_set fds;
	int max_fd = 0;
	while (1) {

		tv.tv_sec = 60;
		tv.tv_usec = 0;
		FD_ZERO(&fds);
		FD_SET(pipefd[0], &fds);
		if (pipefd[0] > max_fd)
			max_fd = pipefd[0];
		
		if (select(max_fd + 1, &fds, NULL, NULL, &tv) < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
		}
		if (FD_ISSET(pipefd[0], &fds)) {
			char signals[100];
			ret = recv(pipefd[0], signals, sizeof(signals), 0);
			if (ret > 0) {
				for(i = 0; i < ret; i++) {
					switch(signals[i]) {
					case SIGTERM:
					case SIGINT:
						msg_final(); 
						exit(0);
						break;
					}
				}
			}
		}
	}

	return 0;

}
