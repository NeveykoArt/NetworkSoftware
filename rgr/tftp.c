#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <ctype.h>
#include <netdb.h>

#define TFTP_PORT 69
#define BUFFER_SIZE 516
#define DATA_SIZE 512
#define TIMEOUT 5
#define MAX_RETRIES 3
#define MAX_FILENAME 256
#define MAX_HOSTNAME 256

#define TFTP_RRQ   1
#define TFTP_WRQ   2
#define TFTP_DATA  3
#define TFTP_ACK   4
#define TFTP_ERROR 5

struct tftp_packet {
	uint16_t opcode;
	union {
		struct {
			uint16_t block_num;
			uint8_t data[DATA_SIZE];
		} data;
		struct {
			uint16_t block_num;
		} ack;
		struct {
			uint16_t error_code;
			char error_msg[BUFFER_SIZE];
		} error;
		char raw[BUFFER_SIZE];
	};
};

struct tftp_client_state {
	char server_host[MAX_HOSTNAME];
	struct sockaddr_in server_addr;
	int connected;
	int sockfd;
};

struct tftp_client_state client_state = {
	.connected = 0,
	.sockfd = -1
};

void print_help(void);
void print_status(void);
int connect_server(const char *hostname);
int disconnect_server(void);
int tftp_get(const char *remote_file, const char *local_file);
int tftp_put(const char *local_file, const char *remote_file);
void interactive_mode(void);
void parse_command(char *command);
void trim_newline(char *str);

int create_rrq_packet(struct tftp_packet *packet, const char *filename) {
	packet->opcode = htons(TFTP_RRQ);

	char *ptr = packet->raw;

	strcpy(ptr, filename);
	ptr += strlen(filename);

	*ptr = '\0';
	ptr++;

	strcpy(ptr, "octet");
	ptr += strlen("octet");

	*ptr = '\0';

	return (ptr - packet->raw) + sizeof(uint16_t) + 1;
}

int create_wrq_packet(struct tftp_packet *packet, const char *filename) {
	packet->opcode = htons(TFTP_WRQ);

	char *ptr = packet->raw;

	strcpy(ptr, filename);
	ptr += strlen(filename);

	*ptr = '\0';
	ptr++;

	strcpy(ptr, "octet");
	ptr += strlen("octet");

	*ptr = '\0';

	return (ptr - packet->raw) + sizeof(uint16_t) + 1;
}

int create_ack_packet(struct tftp_packet *packet, uint16_t block_num) {
	packet->opcode = htons(TFTP_ACK);
	packet->ack.block_num = htons(block_num);
	return 4;
}

int tftp_get(const char *remote_file, const char *local_file) {
	if (!client_state.connected) {
		printf("Не подключен к серверу. Используйте 'connect <hostname>'\n");
        	return -1;
	}

	struct tftp_packet send_packet, recv_packet;
	struct sockaddr_in from_addr;
	socklen_t addr_len;
	FILE *output_file;

	printf("Скачивание файла '%s' как '%s'\n", remote_file, local_file);

	int packet_len = create_rrq_packet(&send_packet, remote_file);

	if (sendto(client_state.sockfd, &send_packet, packet_len, 
                        0, (struct sockaddr *)&client_state.server_addr, 
                            sizeof(client_state.server_addr)) < 0) {
		perror("Ошибка отправки RRQ");
		return -1;
	}

	addr_len = sizeof(from_addr);

	struct timeval tv;
	tv.tv_sec = TIMEOUT;
	tv.tv_usec = 0;
	setsockopt(client_state.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	int recv_len = recvfrom(client_state.sockfd, &recv_packet, BUFFER_SIZE, 
        0, (struct sockaddr *)&from_addr, &addr_len);

	if (recv_len < 0) {
		perror("Таймаут при ожидании ответа");
		return -1;
	}

	uint16_t opcode = ntohs(recv_packet.opcode);

	if (opcode == TFTP_ERROR) {
		uint16_t error_code = ntohs(recv_packet.error.error_code);
		printf("Ошибка TFTP: %d - %s\n", error_code, recv_packet.error.error_msg);
		return -1;
	}

	if (opcode != TFTP_DATA) {
		printf("Неожиданный ответ: opcode=%d (ожидался DATA)\n", opcode);
		return -1;
	}

	output_file = fopen(local_file, "wb");
    if (!output_file) {
		perror("Ошибка создания файла");
		return -1;
	}

	uint16_t expected_block = 1;
    int total_bytes = 0;
    int retry_count = 0;

    while (1) {
        uint16_t block_num = ntohs(recv_packet.data.block_num);

        if (block_num == expected_block) {
            int data_len = recv_len - 4;
            fwrite(recv_packet.data.data, 1, data_len, output_file);
            total_bytes += data_len;

            printf("Получен блок %d (%d байт)\n", block_num, data_len);

            create_ack_packet(&send_packet, block_num);
            sendto(client_state.sockfd, &send_packet, 4, 0, 
                (struct sockaddr *)&from_addr, addr_len);

            if (data_len < DATA_SIZE) {
                printf("Скачивание завершено. Всего байт: %d\n", total_bytes);
                break;
            }

            expected_block++;
            retry_count = 0;
        }

        tv.tv_sec = TIMEOUT;
        tv.tv_usec = 0;
        setsockopt(client_state.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        recv_len = recvfrom(client_state.sockfd, &recv_packet, 
            BUFFER_SIZE, 0, (struct sockaddr *)&from_addr, &addr_len);

        if (recv_len < 0) {
            retry_count++;
            if (retry_count > MAX_RETRIES) {
                printf("Таймаут: превышено количество попыток\n");
                break;
            }

            create_ack_packet(&send_packet, expected_block - 1);
            sendto(client_state.sockfd, &send_packet, 4, 0, 
                (struct sockaddr *)&from_addr, addr_len);

            recv_len = recvfrom(client_state.sockfd, &recv_packet, 
                BUFFER_SIZE, 0, (struct sockaddr *)&from_addr, &addr_len);
        } else {
            retry_count = 0;
        }
    }
    
    fclose(output_file);
    return total_bytes;
}

int tftp_put(const char *local_file, const char *remote_file) {
    if (!client_state.connected) {
        printf("Не подключен к серверу. Используйте 'connect <hostname>'\n");
        return -1;
    }

    struct tftp_packet send_packet, recv_packet;
    struct sockaddr_in from_addr;
    socklen_t addr_len;
    FILE *input_file;

    printf("Загрузка файла '%s' как '%s'\n", local_file, remote_file);

    input_file = fopen(local_file, "rb");
    if (!input_file) {
        printf("Ошибка: файл '%s' не существует\n", local_file);
        fclose(input_file);
        return -1;
    }

    int packet_len = create_wrq_packet(&send_packet, remote_file);

    if (sendto(client_state.sockfd, &send_packet, packet_len, 
            0, (struct sockaddr *)&client_state.server_addr, 
                sizeof(client_state.server_addr)) < 0) {
        perror("Ошибка отправки WRQ");
        return -1;
    }

    addr_len = sizeof(from_addr);
    struct timeval tv;
    tv.tv_sec = TIMEOUT;
    tv.tv_usec = 0;
    setsockopt(client_state.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int recv_len = recvfrom(client_state.sockfd, &recv_packet, 
        BUFFER_SIZE, 0, (struct sockaddr *)&from_addr, &addr_len);

    if (recv_len < 0) {
        perror("Таймаут при ожидании ответа на WRQ");
        return -1;
    }

    uint16_t opcode = ntohs(recv_packet.opcode);

    if (opcode == TFTP_ERROR) {
        uint16_t error_code = ntohs(recv_packet.error.error_code);
        printf("Ошибка TFTP: %d - %s\n", error_code, recv_packet.error.error_msg);
        return -1;
    }

    if (opcode != TFTP_ACK || ntohs(recv_packet.ack.block_num) != 0) {
        printf("Неожиданный ответ на WRQ\n");
        return -1;
    }

    uint16_t block_num = 1;
    int total_bytes = 0;
    int retry_count = 0;

    while (1) {
        send_packet.opcode = htons(TFTP_DATA);
        send_packet.data.block_num = htons(block_num);

        int bytes_read = fread(send_packet.data.data, 1, DATA_SIZE, input_file);
        total_bytes += bytes_read;

        int data_packet_len = 4 + bytes_read;

        printf("Отправляем блок %d (%d байт)\n", block_num, bytes_read);

        if (sendto(client_state.sockfd, &send_packet, data_packet_len, 
                0, (struct sockaddr *)&from_addr, addr_len) < 0) {
            perror("Ошибка отправки данных");
            break;
        }

        tv.tv_sec = TIMEOUT;
        tv.tv_usec = 0;
        setsockopt(client_state.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        recv_len = recvfrom(client_state.sockfd, &recv_packet, 
            BUFFER_SIZE, 0, (struct sockaddr *)&from_addr, &addr_len);

        if (recv_len < 0) {
            retry_count++;
            if (retry_count > MAX_RETRIES) {
                printf("Таймаут: превышено количество попыток\n");
                break;
            }

            printf("Таймаут, повторная отправка блока %d\n", block_num);
            continue;
        }

        retry_count = 0;
        opcode = ntohs(recv_packet.opcode);

        if (opcode == TFTP_ACK) {
            uint16_t ack_block = ntohs(recv_packet.ack.block_num);
            if (ack_block == block_num) {
                printf("Получен ACK для блока %d\n", block_num);
                block_num++;

                if (bytes_read < DATA_SIZE) {
                    printf("Загрузка завершена. Всего байт: %d\n", total_bytes);
                    break;
                }
            }
        } else if (opcode == TFTP_ERROR) {
            printf("Ошибка TFTP: %d - %s\n", 
            ntohs(recv_packet.error.error_code), 
            recv_packet.error.error_msg);
            break;
        }
    }

    fclose(input_file);
    return total_bytes;
}

int connect_server(const char *hostname) {
    if (client_state.connected) {
        disconnect_server();
    }

    client_state.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_state.sockfd < 0) {
        perror("Ошибка создания сокета");
        return -1;
    }

    memset(&client_state.server_addr, 0, sizeof(client_state.server_addr));
    client_state.server_addr.sin_family = AF_INET;
    client_state.server_addr.sin_port = htons(TFTP_PORT);

    if (inet_pton(AF_INET, hostname, &client_state.server_addr.sin_addr) <= 0) {
        struct hostent *host = gethostbyname(hostname);
        if (!host) {
            printf("Не удалось разрешить hostname: %s\n", hostname);
            close(client_state.sockfd);
            client_state.sockfd = -1;
            return -1;
        }
        memcpy(&client_state.server_addr.sin_addr, 
        host->h_addr_list[0], host->h_length);
    }

    strncpy(client_state.server_host, hostname, MAX_HOSTNAME - 1);
    client_state.server_host[MAX_HOSTNAME - 1] = '\0';
    client_state.connected = 1;

    printf("Подключено к %s\n", hostname);
    return 0;
}

int disconnect_server(void) {
    if (client_state.sockfd >= 0) {
        close(client_state.sockfd);
        client_state.sockfd = -1;
    }
    client_state.connected = 0;
    printf("Отключено от сервера\n");
    return 0;
}

void print_help(void) {
    printf("TFTP Client\n");
    printf("Доступные команды:\n\n");
    printf("connect     подключиться к удаленному TFTP серверу\n");
    printf("put         отправить файл на сервер\n");
    printf("get         получить файл с сервера\n");
    printf("quit        выйти из TFTP клиента\n");
    printf("status      показать текущий статус\n");
    printf("?           показать эту справку\n");
    printf("help        показать эту справку\n");
}

void print_status(void) {
    printf("Текущий статус:\n");
    printf("  Подключение: %s\n", 
           client_state.connected ? client_state.server_host : "не подключено");
    printf("  Режим: octet (бинарный)\n");
}

void trim_newline(char *str) {
    int len = strlen(str);
    if (len > 0 && str[len-1] == '\n') {
        str[len-1] = '\0';
    }
}

void parse_command(char *command) {
    char cmd[64];
    char arg1[256] = "";
    char arg2[256] = "";
    
    sscanf(command, "%63s %255s %255s", cmd, arg1, arg2);
    
    for (int i = 0; cmd[i]; i++) {
        cmd[i] = tolower(cmd[i]);
    }
    
    if (strcmp(cmd, "?") == 0 || strcmp(cmd, "help") == 0) {
        print_help();
    }
    else if (strcmp(cmd, "connect") == 0 || strcmp(cmd, "c") == 0) {
        if (strlen(arg1) == 0) {
            printf("Использование: connect <hostname>\n");
        } else {
            connect_server(arg1);
        }
    }
    else if (strcmp(cmd, "put") == 0 || strcmp(cmd, "p") == 0) {
        if (strlen(arg1) == 0) {
            printf("Использование: put <локальный_файл> [удаленный_файл]\n");
        } else {
            const char *remote = (strlen(arg2) > 0) ? arg2 : arg1;
            tftp_put(arg1, remote);
        }
    }
    else if (strcmp(cmd, "get") == 0 || strcmp(cmd, "g") == 0) {
        if (strlen(arg1) == 0) {
            printf("Использование: get <удаленный_файл> [локальный_файл]\n");
        } else {
            const char *local = (strlen(arg2) > 0) ? arg2 : arg1;
            tftp_get(arg1, local);
        }
    }
    else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0 || 
             strcmp(cmd, "exit") == 0) {
        disconnect_server();
        exit(0);
    }
    else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "st") == 0) {
        print_status();
    }
    else if (strlen(cmd) > 0) {
        printf("Неизвестная команда: '%s'. Введите 'help' для списка команд.\n", cmd);
    }
}

void interactive_mode(void) {
    char input[512];
    
    printf("TFTP Client\n");
    printf("Введите 'help' для списка команд\n\n");
    
    while (1) {
        printf("tftp> ");
        fflush(stdout);
        
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        trim_newline(input);
        
        if (strlen(input) == 0) {
            continue;
        }
        
        parse_command(input);
    }
    
    printf("\n");
    disconnect_server();
}

void batch_mode(int argc, char *argv[]) {
    if (argc != 5) {
        printf("Использование: %s <сервер> <get|put> <файл1> <файл2>\n", argv[0]);
        printf("Примеры:\n");
        printf("  %s 127.0.0.1 get server.txt local.txt\n", argv[0]);
        printf("  %s 127.0.0.1 put local.txt server.txt\n", argv[0]);
        exit(1);
    }
    
    const char *server_ip = argv[1];
    const char *command = argv[2];
    
    if (connect_server(server_ip) < 0) {
        exit(1);
    }
    
    if (strcmp(command, "get") == 0) {
        const char *remote_file = argv[3];
        const char *local_file = argv[4];
        
        printf("Скачивание файла '%s' с сервера %s как '%s'\n", 
               remote_file, server_ip, local_file);
        
        int result = tftp_get(remote_file, local_file);
        if (result > 0) {
            printf("Файл успешно скачан (%d байт)\n", result);
        } else {
            printf("Ошибка скачивания файла\n");
            disconnect_server();
            exit(1);
        }
        
    } else if (strcmp(command, "put") == 0) {
        const char *local_file = argv[3];
        const char *remote_file = argv[4];
        
        printf("Загрузка файла '%s' на сервер %s как '%s'\n", 
               local_file, server_ip, remote_file);
        
        int result = tftp_put(local_file, remote_file);
        if (result > 0) {
            printf("Файл успешно загружен (%d байт)\n", result);
        } else {
            printf("Ошибка загрузки файла\n");
            disconnect_server();
            exit(1);
        }
        
    } else {
        printf("Неизвестная команда: %s\n", command);
        printf("Используйте 'get' или 'put'\n");
        disconnect_server();
        exit(1);
    }
    
    disconnect_server();
}

int main(int argc, char *argv[]) {
    if (argc == 1) {
        interactive_mode();
    } else if (argc == 2 && (strcmp(argv[1], "-i") == 0 || 
                            strcmp(argv[1], "--interactive") == 0)) {
        interactive_mode();
    } else {
        batch_mode(argc, argv);
    }
    
    return 0;
}
