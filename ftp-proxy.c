#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define PROXY_LISTEN_PORT 21
#define BUFFSIZE 4096
#define IP_LEN	20
#define MAX_PATH_LEN 255
#define MAX_FILE_CACHE_NUMBER 1024

int proxy_cmd_socket    = 0;    
int accept_cmd_socket   = 0; 
int connect_cmd_socket  = 0;
int proxy_data_socket   = 0;
int accept_data_socket  = 0;
int connect_data_socket = 0;

char proxy_server_ip[IP_LEN];
char proxy_client_ip[IP_LEN];   
char server_ip[IP_LEN];

unsigned short server_port = 0;
unsigned short proxy_data_port = 0;
unsigned short remote_data_port = 0;

unsigned short trans_mode = 0;//0:port 1:pasv
unsigned short cache_status = 0; //0:none 1: no cache 2:have cache

fd_set main_set, work_set;
char current_work_dir[MAX_PATH_LEN];
char current_filename[MAX_PATH_LEN];
char local_cache_pos[MAX_PATH_LEN];

char client_cmd_port_or_pasv_buffer[BUFFSIZE] = {0};//缓存PORT命令和PASV命令，根据他们命令之后的一个命令来决定怎么做

struct FileCache
{
	char *data_buffer;
	int data_count;
	struct FileCache *next;
};

char file_cache[MAX_FILE_CACHE_NUMBER][MAX_PATH_LEN];
unsigned short current_file_cache_count = 0;
struct FileCache* file_data_cache[MAX_FILE_CACHE_NUMBER];

int get_max_nfd()
{
	int max_nfd = 0;
	if (max_nfd < proxy_cmd_socket)
	{
		max_nfd = proxy_cmd_socket;
	}
	if (max_nfd < accept_cmd_socket)
	{
		max_nfd = accept_cmd_socket;
	}
	if (max_nfd < connect_cmd_socket)
	{
		max_nfd = connect_cmd_socket;
	}
	if (max_nfd < proxy_data_socket)
	{
		max_nfd = proxy_data_socket;
	}
	if (max_nfd < accept_data_socket)
	{
		max_nfd = accept_data_socket;
	}
	if (max_nfd < connect_data_socket)
	{
		max_nfd = connect_data_socket;
	}
	return max_nfd + 1;
}

void close_socket(int* fd)
{
	if (*fd != 0)
	{
		close(*fd);
		FD_CLR(*fd, &main_set);
		*fd = 0;
	}
}

void disconnect_client()
{
	if (accept_cmd_socket != 0)
	{
		printf("disconnect client\n");
	}

	close_socket(&accept_cmd_socket);
	close_socket(&connect_cmd_socket);
	close_socket(&proxy_data_socket);
	close_socket(&accept_data_socket);
	close_socket(&connect_data_socket);

	bzero(current_work_dir, MAX_PATH_LEN);
	bzero(current_filename, MAX_PATH_LEN);
	bzero(client_cmd_port_or_pasv_buffer, BUFFSIZE);
}

int listen_socket(unsigned short port)
{
	int fd;
	struct sockaddr_in addr;
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		printf("socket() failed\n");
		return -1;
	}

	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		printf("socket() failed\n");
		return -1;
	}

	if (listen(fd, 20) < 0)
	{
		printf("listen() failed\n");
		return -1;
	}

	return fd;
}

int connect_server(char *ip, unsigned short port)
{
	int fd = 0;
	struct sockaddr_in servaddr;
	bzero(&servaddr,sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port);
	inet_pton(AF_INET, ip, &servaddr.sin_addr);

	fd = socket(AF_INET,SOCK_STREAM,0);
	if (fd < 0)
	{
		printf("socket() failed \n");
		return -1;
	}

	if (connect(fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0)
	{
		printf("socket() failed \n");
		return -1;
	}

	return fd;
}

int get_socket_local_info(int fd, char *ip, int ipBufferSize, unsigned short  *port)
{
	char *p = 0;
	struct sockaddr_in addr;
	socklen_t addrlen;
	bzero(ip,ipBufferSize);
	addrlen = sizeof(addr);
	if (getsockname(fd, (struct sockaddr *)&addr, &addrlen) < 0)
	{
		printf("getsockname() failed\n");
		return 0;
	}

	if (ip)
	{
		inet_ntop(AF_INET, &addr.sin_addr, ip, addrlen);
		p = ip;
		while(*p){
			if(*p == '.') *p = ',';
			p++;
		}
	}
	if (port)
	{
		*port = ntohs(addr.sin_port);
	}

	return 1;
}


void check_proxy_cmd_socket_read()
{
	if (!FD_ISSET(proxy_cmd_socket, &work_set))
	{
		return;
	}

	if (accept_cmd_socket != 0)
	{
		disconnect_client();
		return;
	}

	printf("new accept\n");
	accept_cmd_socket  = accept(proxy_cmd_socket, NULL, NULL);
	if (accept_cmd_socket < 1)
	{
		printf("accept() faile%d\n", proxy_cmd_socket);
		disconnect_client();
		return;
	}

	if (get_socket_local_info(accept_cmd_socket, proxy_client_ip, IP_LEN, NULL) <= 0)
	{
		printf("get_socket_local_info() failed\n");
		disconnect_client();
		return;
	}

	connect_cmd_socket = connect_server(server_ip, server_port);
	if (connect_cmd_socket < 1)
	{
		printf("connect_server() failed\n");
		disconnect_client();
		return;
	}

	if (get_socket_local_info(connect_cmd_socket, proxy_server_ip, IP_LEN, NULL) <= 0)
	{
		printf("get_socket_local_info() failed\n");
		disconnect_client();
		return;
	}

	FD_SET(accept_cmd_socket, &main_set);
	FD_SET(connect_cmd_socket, &main_set);
}


void parse_cmd(char *buff, char **cmd, char **param)
{
	int i;
	char *p;
	while((p = &buff[strlen(buff) - 1])
		&& (*p == '\r' || *p == '\n'))
	{
		*p = 0;
	}

	p = strchr(buff,' ');
	*cmd = buff;
	if(!p)
	{
		*param = NULL;
	}
	else
	{
		*p = 0;
		*param = p + 1;
	}

	for(i = 0; i < strlen(*cmd); i++)
	{
		(*cmd)[i] = toupper((*cmd)[i]);
	}
}

unsigned short get_port_from_param(char *param)
{
	unsigned short port,t;
	int count = 0;
	char *p = param;    

	while(count < 4)
	{
		if(*(p++) == ',')
		{
			count++;
		}
	}
	sscanf(p,"%hu",&port);

	while(*p != ',' && *p != '\r' && *p != ')')
	{
		p++;
	}

	if (*p == ',') 
	{
		p++;
		sscanf(p,"%hu",&t);
		port = port * 256 + t;
	}
	return port;
}

int connect_client_by_sockaddr(struct sockaddr_in servaddr)
{
	int fd = 0;
	struct sockaddr_in cliaddr;
	bzero(&cliaddr,sizeof(cliaddr));
	cliaddr.sin_family = AF_INET;
	cliaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		printf("socket() failed\n");
		return -1;
	}

	if(bind(fd,(struct sockaddr *)&cliaddr,sizeof(cliaddr) ) < 0){
		printf("bind() failed\n");
		return -1;
	}

	servaddr.sin_family = AF_INET;
	if(connect(fd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0){
		printf("connect() failed\n");
		return -1;
	}
	return fd;
}

int is_img_or_pdf_file(char* filename)
{
	char *p = 0;
	int count = 0;
	count = strlen(filename);
	if (count <= 4)
	{
		return 0;
	}
	p = filename + count - 4;
	if (strcmp(p, ".pdf") == 0 ||
		strcmp(p, ".jpg") == 0 ||
		strcmp(p, ".bmp") == 0 ||
		strcmp(p, ".png") == 0 ||
		strcmp(p, ".gif") == 0)
	{
		return 1;
	}
	return 0;
}

int have_file_cache(char *filename)
{
	char path[MAX_PATH_LEN] = {0};
	
	memcpy(path,local_cache_pos,strlen(local_cache_pos));
	memcpy(path+strlen(path),filename,strlen(filename));
	if(access(path,0)==-1)
	{
		return -1;
	}
	current_file_cache_count = 2;
	return 1;
}

void close_data_connect()
{
	close_socket(&proxy_data_socket);
	close_socket(&accept_data_socket);
	close_socket(&connect_data_socket);

	printf("close data connect\n");
}

int send_cache_file_to_client(int fd, struct FileCache* file_buffer)
{
	char data_buffer[BUFFSIZE] = {0};
	char path[MAX_PATH_LEN] = {0};
	int file_fd = 0;
	int baytes_read = 0;
	
	memcpy(path,local_cache_pos,strlen(local_cache_pos));
	memcpy(path+strlen(path),current_filename,strlen(current_filename));
	printf("1111\n");
	if(access(path,0)==-1)
	{
		printf("can't find the cache %s\n",path);
		return -1;
	}
	printf("22222\n");
	if((file_fd = open(path,O_RDONLY))<0)
	{
		printf("can't find the cache %s\n",path);
		return -1;
	}
	printf("will transform cache");
	while(baytes_read=read(file_fd,data_buffer,BUFFSIZE))
	{
		if(baytes_read<0)
			break;
		if(baytes_read >0)
		{
			if (write(fd, data_buffer, baytes_read) <= 0)
			{
				close_data_connect();
				return -1;
			}
		}
		bzero(data_buffer,BUFFSIZE);
	}

	close_data_connect();
	if (write(accept_cmd_socket, "226 Successfully\r\n", strlen("226 Successfully\r\n")) <= 0)
	{
		disconnect_client();
	}


	printf("send_cache_file_to_client success!\n");
	close(file_id)
	return 1;
}

int check_port_or_pasv_status(char* buffer)
{
	char server_cmd_buffer[BUFFSIZE] = {0};
	char *client_cmd = 0;
	char *client_param = 0;
	char *ipPos = 0;
	char temp_buff[BUFFSIZE] = {0};
	int i = 0;

	if (strlen(buffer) == 0)
	{
		return 1;
	}
	memcpy(temp_buff, buffer, BUFFSIZE);
	parse_cmd(temp_buff, &client_cmd, &client_param);
	printf("check buffer= %s\n ",buffer);
	if (cache_status == 2)
	{//有缓存
	    printf("aaaaaaaaaaaaaaaaaaaaaaa\n");
		i = atoi(client_param);
		if (i < 0 || i >= current_file_cache_count)
		{
			return -1;
		}
		if (strcmp(client_cmd, "PORT") == 0)
		{
			printf("xxxxxxxxxxxxxxxxxxx\n");
			if (send_cache_file_to_client(connect_data_socket, file_data_cache[i]) <= 0)
			{
				return -1;
			}
		}
		else if (strcmp(client_cmd, "PASV") == 0)
		{
			printf("zzzzzzzzzzzzzzzzzzzzzzzzzzzzz\n");
			accept_data_socket = accept(proxy_data_socket, NULL, NULL);
			if (accept_data_socket < 1)
			{
				return -1;
			}
			printf("kkkkkkkkkkkkkkkkkkkkk\n");
			FD_SET(accept_data_socket, &main_set);
			printf("bbbbbbbbbbbbbbbbbbbbbbb\n");
			if (send_cache_file_to_client(accept_data_socket, file_data_cache[i]) <= 0)
			{
				return -1;
			}
		}
		bzero(buffer, BUFFSIZE);
		return 1;
	}
	else
	{//没有缓存
		if (strcmp(client_cmd, "PORT") == 0)
		{//需要创建一个套接字，并等待服务器连接
			proxy_data_socket = listen_socket(0);
			if (get_socket_local_info(proxy_data_socket, NULL, 0, &proxy_data_port) <= 0)
			{
				return -1;
			}
			bzero(buffer, BUFFSIZE);
			memcpy(buffer, "PORT 127,0,0,1,0,0\r\n", strlen("PORT 127,0,0,1,0,0\r\n"));
			sprintf(buffer + 5, "%s,%d,%d\r\n", proxy_server_ip, proxy_data_port / 256, proxy_data_port % 256);
			if (write(connect_cmd_socket, buffer, strlen(buffer)) <= 0)
			{
				return -1;
			}

			FD_SET(proxy_data_socket, &main_set);
			if(read(connect_cmd_socket, server_cmd_buffer, BUFFSIZE) == 0)
			{
				return -1;
			}

			if (server_cmd_buffer[0] == '2')
			{
				bzero(buffer, BUFFSIZE);
				return 1;//第一个2表示连接成功
			}

			return -1;
		}
		else if (strcmp(client_cmd, "PASV") == 0)
		{
			if (write(connect_cmd_socket, buffer, strlen(buffer)) <= 0)
			{
				return -1;
			}

			if (read(connect_cmd_socket, server_cmd_buffer, BUFFSIZE) <= 0)
			{
				return -1;
			}

			if(server_cmd_buffer[0] == '2' &&
				server_cmd_buffer[1] == '2' && 
				server_cmd_buffer[2] == '7')
			{
				remote_data_port = get_port_from_param(server_cmd_buffer);
				connect_data_socket = connect_server(server_ip, remote_data_port);
				if (accept_data_socket != 0)
				{
					FD_SET(accept_data_socket, &main_set);
				}

				FD_SET(connect_data_socket, &main_set);
				bzero(buffer, BUFFSIZE);
				return 1;

			}
			return -1;
		}
	}
	bzero(buffer, BUFFSIZE);
	return 1;
}

void check_accept_cmd_socket_read()
{
	char client_cmd_buffer[BUFFSIZE] = {0};
	char temp_buff[BUFFSIZE] = {0};
	char *client_cmd = 0;
	char *client_param = 0;
	socklen_t clilent;
	struct sockaddr_in cliaddr;
	char *ipPos = 0;
	char filename[MAX_PATH_LEN] = {0};
	int i = 0;

	if (!FD_ISSET(accept_cmd_socket, &work_set))
	{
		return;
	}

	if (read(accept_cmd_socket, client_cmd_buffer, BUFFSIZE) <= 0)
	{
		disconnect_client();
		return;
	} 
	else
	{
		printf("command received from client : %s\n", client_cmd_buffer);
		memcpy(temp_buff, client_cmd_buffer, BUFFSIZE);
		parse_cmd(temp_buff, &client_cmd, &client_param);

		if (strcmp(client_cmd, "PORT") == 0)
		{
			trans_mode = 0;
			remote_data_port = get_port_from_param(client_param);
			if (remote_data_port <= 0)
			{
				disconnect_client();
				return;
			}

			clilent = sizeof(cliaddr);
			if(getpeername(accept_cmd_socket, (struct sockaddr *)&cliaddr,&clilent) < 0)
			{
				printf("getpeername() failed\n ");
				disconnect_client();
				return;
			}

			cliaddr.sin_port = htons(remote_data_port);
			connect_data_socket = connect_client_by_sockaddr(cliaddr);
			if (connect_data_socket <= 0)
			{
				disconnect_client();
				return;
			}

			memcpy(client_cmd_port_or_pasv_buffer, client_cmd_buffer, strlen(client_cmd_buffer));
			bzero(client_cmd_buffer, BUFFSIZE);
			memcpy(client_cmd_buffer, "200 Port command successful\r\n", strlen("200 Port command successful\r\n"));
			if (write(accept_cmd_socket, client_cmd_buffer, strlen(client_cmd_buffer)) <= 0)
			{
				disconnect_client();
			}
			return;
		}
		else if (strcmp(client_cmd, "PASV") == 0)
		{
			proxy_data_socket = listen_socket(0);
			if (get_socket_local_info(proxy_data_socket, NULL, 0, &proxy_data_port) <= 0)
			{
				printf("get_socket_local_info() failed\n");
				disconnect_client();
				return;
			}
			trans_mode = 1;
			FD_SET(proxy_data_socket, &main_set);
			memcpy(client_cmd_port_or_pasv_buffer, client_cmd_buffer, strlen(client_cmd_buffer));

			bzero(client_cmd_buffer, BUFFSIZE);
			memcpy(client_cmd_buffer, "227 Entering Passive Mode (127,0,0,1,0,0)", strlen("227 Entering Passive Mode (127,0,0,1,0,0)"));
			ipPos = strchr(client_cmd_buffer, '(');
			sprintf(ipPos + 1, "%s,%d,%d)\r\n", proxy_client_ip, proxy_data_port / 256, proxy_data_port % 256);
			if (write(accept_cmd_socket, client_cmd_buffer, strlen(client_cmd_buffer)) <= 0)
			{
				disconnect_client();
			}
			return;
		}
		else if (strcmp(client_cmd, "CWD") == 0)
		{
			if (strlen(client_param) > 0)
			{
				if (client_param[0] == '/')
				{
					memcpy(current_work_dir, client_param, strlen(client_param));
				}
				else
				{
					if (strlen(current_work_dir) == 0)
					{
						current_work_dir[0] = '/';
					}
					else if (current_work_dir[strlen(current_work_dir) -1 ] != '/')
					{
						current_work_dir[strlen(current_work_dir)] = '/';
					}
					memcpy(current_work_dir + strlen(current_work_dir), client_param, strlen(client_param));
				}

			}
			printf("current work dir:%s\n", current_work_dir);

		}
		else if (strcmp(client_cmd, "RETR") == 0)
		{
			cache_status = 0;
			memcpy(filename, current_work_dir, strlen(current_work_dir));
			if (strlen(filename) == 0)
			{
				filename[0] = '/';
			}
			else if (filename[strlen(filename) -1 ] != '/')
			{
				filename[strlen(filename)] = '/';
			}
			memcpy(filename + strlen(filename), client_param, strlen(client_param));
			if (is_img_or_pdf_file(filename))
			{
				i = have_file_cache(filename);
				if (i != -1)
				{
					printf("file exist cache\n");
					cache_status = 2;
					memcpy(current_filename, filename, strlen(filename));
					bzero(client_cmd_port_or_pasv_buffer + 4, BUFFSIZE - 4);
					sprintf(client_cmd_port_or_pasv_buffer + 4, " %d", i);
				}
				else
				{
					printf("file will cache\n");
					cache_status = 1;
					memcpy(current_filename, filename, strlen(filename));
				}
			}
		}
		if (check_port_or_pasv_status(client_cmd_port_or_pasv_buffer) <= 0)
		{
			disconnect_client();
			return;
		}

		if (cache_status != 2)
		{//不向服务器发送下载文件请求
			if (write(connect_cmd_socket, client_cmd_buffer, strlen(client_cmd_buffer)) <=0)
			{
				disconnect_client();
				return;
			}
		}
		else
		{
			cache_status = 0;
		}
	}
}

void check_connect_cmd_socket_read()
{
	char server_cmd_buffer[BUFFSIZE] = {0};
	if (!FD_ISSET(connect_cmd_socket, &work_set))
	{
		return;
	}

	if(read(connect_cmd_socket, server_cmd_buffer,BUFFSIZE) <= 0)
	{
		disconnect_client();
		return ;
	}

	printf("reply received from server : %s\n", server_cmd_buffer);

	if (write(accept_cmd_socket,server_cmd_buffer,strlen(server_cmd_buffer)) <= 0)
	{
		disconnect_client();
	}
}

void check_proxy_data_socket_read()
{
	if (!FD_ISSET(proxy_data_socket, &work_set))
	{
		return;
	}
	if (trans_mode)
	{//pasv
		accept_data_socket = accept(proxy_data_socket, NULL, NULL);
		if (accept_data_socket < 1)
		{
			disconnect_client();
		}
		//连接到服务器才把客户端的这个加到监控里面，如果这里没加，在连接到服务器的时候会加上
		if (connect_data_socket != 0)
		{
			FD_SET(accept_data_socket, &main_set);
		}
	}
	else
	{//port
		accept_data_socket = accept(proxy_data_socket, NULL, NULL); 
		if (accept_data_socket < 1)
		{
			disconnect_client();
		}
		FD_SET(accept_data_socket, &main_set);
		FD_SET(connect_data_socket, &main_set);
	}
}

void save_file_to_cache(struct FileCache* head)
{
	struct FileCache *next = 0, *current = 0;
	int cache_pos = current_file_cache_count;
	if (current_file_cache_count >= MAX_FILE_CACHE_NUMBER)
	{
		//如果缓存满了，则产生一个随机位置，删除该位置的缓存
		cache_pos = rand() % MAX_FILE_CACHE_NUMBER;
		current = file_data_cache[cache_pos];
		while (current)
		{
			free(current->data_buffer);
			next = current->next;
			free(current);
			current = next;
		}
	}
	else
	{
		current_file_cache_count++;
	}

	memcpy(file_cache[cache_pos], current_filename, strlen(current_filename));
	file_data_cache[cache_pos] = head;

	cache_status = 0;//缓存完毕
}

void save_file_to_local(struct FileCache* head)
{
	char dir[MAX_PATH_LEN] = {0};
	char path[MAX_PATH_LEN] = {0};
	int p = 1, len = 0;
	int	fd = 0;
	memcpy(path, local_cache_pos, strlen(local_cache_pos));
	memcpy(path + strlen(path), current_filename, strlen(current_filename));
	printf("save file to local,file path is %s\n", path);

	len = strlen(path);
	dir[0] = '/';
	while(p < len)
	{
		if (path[p] == '/')
		{
			if (access(dir, 0) == -1)
			{
				if (mkdir(dir, 0777) == -1)
				{
					printf("create dir failed, path:%s\n", dir);
					return ;
				}
			}
		}
		dir[p] = path[p];
		p++;
	}

	if ((fd = open(path, O_RDWR|O_CREAT)) < 0)
	{
		printf("craete cache file error\n");
	}

	while (head != NULL && head->data_count != 0)
	{
		write(fd, head->data_buffer, head->data_count);
		head = head->next;
	}
	printf("cache succeed\n");
	close(fd);
}

void check_accept_data_socket_read()
{
	struct FileCache* head = 0, *next = 0, *current = 0;
	if (!FD_ISSET(accept_data_socket, &work_set))
	{
		return;
	}

	head = (struct FileCache*)malloc(sizeof(struct FileCache));
	head->data_buffer = (char*)malloc(BUFFSIZE);
	head->data_count = 0;
	head->next = 0;
	next = head;
	current = next;

	do 
	{
		current->data_count = read(accept_data_socket, current->data_buffer, BUFFSIZE);
		if (current->data_count <= 0)
		{//done
			if (cache_status != 1)
			{
				current = head;
				while (current)
				{
					free(current->data_buffer);
					next = current->next;
					free(current);
					current = next;
				}
				close_data_connect();
			}
			else
			{
				save_file_to_cache(head);
				save_file_to_local(head);
			}

			break;
		}
		else
		{
			if (write(connect_data_socket, current->data_buffer, current->data_count) <= 0)
			{//error, free
				current = head;
				while (current)
				{
					free(current->data_buffer);
					next = current->next;
					free(current);
					current = next;
				}
				close_data_connect();
				break;
			}
		}

		next = (struct FileCache*)malloc(sizeof(struct FileCache));
		next->data_buffer = (char*)malloc(BUFFSIZE);
		next->data_count = 0;
		next->next = 0;
		current->next = next;
		current = next;
	} while (1);
}

void check_connect_data_socket_read()
{
	struct FileCache* head = 0, *next = 0, *current = 0;
	if (!FD_ISSET(connect_data_socket, &work_set))
	{
		return;
	}

	head = (struct FileCache*)malloc(sizeof(struct FileCache));
	head->data_buffer = (char*)malloc(BUFFSIZE);
	head->data_count = 0;
	head->next = 0;
	next = head;
	current = next;

	do 
	{
		current->data_count = read(connect_data_socket, current->data_buffer, BUFFSIZE);
		if (current->data_count <= 0)
		{//done
			if (cache_status != 1)
			{
				current = head;
				while (current)
				{
					free(current->data_buffer);
					next = current->next;
					free(current);
					current = next;
				}
				close_data_connect();
			}
			else
			{
				save_file_to_cache(head);
				save_file_to_local(head);
			}

			break;
		}
		else
		{
			if (write(accept_data_socket, current->data_buffer, current->data_count) <= 0)
			{//error, free
				current = head;
				while (current)
				{
					free(current->data_buffer);
					next = current->next;
					free(current);
					current = next;
				}
				close_data_connect();
				break;
			}
		}

		next = (struct FileCache*)malloc(sizeof(struct FileCache));
		next->data_buffer = (char*)malloc(BUFFSIZE);
		next->data_count = 0;
		next->next = 0;
		current->next = next;
		current = next;
	} while (1);
}


int main(int argc, const char *argv[])
{
	struct timeval timeout;
	int selectResult = 0;

	if (argc < 4)//参数小于三个则返回
	{
		printf("run error : for example: proxy 127.0.0.1 111  /home\n");
		exit(1);
	}
	strcpy(server_ip, argv[1]);//第一个参数是服务器IP地址
	server_port = atoi(argv[2]);//第二个参数是服务器端口号
	strcpy(local_cache_pos, argv[3]);
	bzero(&timeout, sizeof(timeout));
	bzero(current_work_dir, MAX_PATH_LEN);
	bzero(current_filename, MAX_PATH_LEN);

	proxy_cmd_socket = listen_socket(PROXY_LISTEN_PORT);//监听端口，等待客户端来连接
	if (proxy_cmd_socket <= 0)
	{
		printf("ftp proxy listen port error\n");
		exit(1);
	}
	FD_ZERO(&main_set);
	FD_SET(proxy_cmd_socket, &main_set);

	printf("run...\n");
	while (1)
	{
		timeout.tv_sec = 120;//time out
		timeout.tv_usec = 0;
		FD_ZERO(&work_set);
		memcpy(&work_set, &main_set, sizeof(main_set));

		//异步判断哪个套接字可以读取数据或者接受连接请求
		selectResult = select(get_max_nfd(), &work_set, NULL, NULL, &timeout);

		if (selectResult < 0)
		{
			printf("select() failed\n");
			exit(1);
		}
		else if (selectResult == 0)
		{
			printf("select() timed out.\n");
			disconnect_client();
			continue;
		}

		//分别判断六个套接字有没有数据来临
		check_proxy_cmd_socket_read();
		check_accept_cmd_socket_read();
		check_connect_cmd_socket_read();
		check_proxy_data_socket_read();
		check_accept_data_socket_read();
		check_connect_data_socket_read();
	}
	close(proxy_cmd_socket);
	return 0;
}

