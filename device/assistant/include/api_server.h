#ifndef API_SERVER_H
#define API_SERVER_H

int api_server_start(void);
void api_server_stop(void);
void api_server_write_status(void);
void api_server_write_config(void);
void api_server_check_commands(void);

#endif
