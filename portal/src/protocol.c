#include "connection.h"
#include "protocol.h"

#define PRO_SYS_QUERY  "query"
#define PRO_SYS_AUTH  "auth"


extern int cgi_sys_query_handler(connection_t *con);   //用户查询
extern int cgi_sys_auth_handler(connection_t *con);				//用户认证


static cgi_protocol_t pro_list[] ={
	{PRO_SYS_QUERY, cgi_sys_query_handler},
	{PRO_SYS_AUTH,cgi_sys_auth_handler},
	{NULL,NULL},
};

int cgi_protocol_handler(connection_t *con)
{
	cJSON *obj;
	cgi_protocol_t *cur_protocol;
	char* opt = con_value_get(con,"opt");
	//char* fname = con_value_get(con,"fname");

	if(!opt)
		return PRO_BASE_ARG_ERR; 

	cur_protocol = find_pro_handler(opt);
	if(!cur_protocol)
		return PRO_BASE_ARG_ERR;

/*

	cJSON_AddStringToObject(response,"opt", opt);

	if (connection_is_set(con) == -1)
		return PRO_BASE_ARG_ERR; 
	if (connection_is_set(con)) {
		cJSON_AddStringToObject(response,"function", "set");
	} else {
		cJSON_AddStringToObject(response,"function", "get");
	}
*/
	return cur_protocol->handler(con);
}

cgi_protocol_t *find_pro_handler(const char *pro_opt)
{
    int i;
    if(pro_opt == NULL)
        return NULL;
    i = 0;
    while(1){
        if(pro_list[i].name == NULL)
            return NULL;
        if(strcmp(pro_list[i].name, pro_opt) == 0){
            return &pro_list[i];
        }
        i++;
    }
        return NULL;
}