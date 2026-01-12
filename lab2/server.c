#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_UDP_PACKET_SIZE 1024
#define TCP_BACKLOG 10

static const char *ACK = "ACK";
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *common_tcp_log_file = NULL;

static int max(const int *array, int size) {
    int max_val = array[0];
    for (int i = 1; i < size; i++) {
        if (array[i] > max_val) {
            max_val = array[i];
        }
    }
    return max_val;
}

static int* parse_packet_positions(const char *str, int *outSize) {
    *outSize = 0;
    size_t len = strlen(str);

    if (len < 2 || str[0] != '[' || str[len - 1] != ']') {
        fprintf(stderr, "Wrong input format, must be enclosed in []\n");
        return NULL;
    }

    char *inner = malloc(len - 1);
    if (!inner) {
        perror("malloc");
        return NULL;
    }
    strncpy(inner, str + 1, len - 2);
    inner[len - 2] = '\0';

    int count = 1;
    for (char *p = inner; *p; p++) {
        if (*p == ',') count++;
    }

    int *values = malloc(sizeof(int) * count);
    if (!values) {
        perror("malloc");
        free(inner);
        return NULL;
    }

    int idx = 0;
    char *token = strtok(inner, ",");
    while (token) {
        char *endptr;
        long val = strtol(token, &endptr, 10);
        if (*endptr != '\0') {
            fprintf(stderr, "Invalid number in input: '%s'\n", token);
            free(inner);
            free(values);
            return NULL;
        }
        values[idx++] = (int)val;
        token = strtok(NULL, ",");
    }

    int maxValue = max(values, idx);
    int *result = calloc(maxValue + 1, sizeof(int));
    if (!result) {
        perror("calloc");
        free(inner);
        free(values);
        return NULL;
    }

    for (int i = 0; i < idx; i++) {
        if (values[i] >= 0)
            ++result[values[i]];
    }

    free(inner);
    free(values);
    *outSize = maxValue + 1;
    return result;
}

void *tcp_client_handler(void *arg) {
    int sockfd = *(int*)arg;
    free(arg);

    char buffer[1024];
    ssize_t n;

    while ((n = read(sockfd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = '\0';
        pthread_mutex_lock(&file_mutex);
        if (common_tcp_log_file) {
            fprintf(common_tcp_log_file, "%s\n", buffer);
            fflush(common_tcp_log_file);
        }
        pthread_mutex_unlock(&file_mutex);
    }
    close(sockfd);
    pthread_exit(NULL);
    return NULL;
}

static int setup_udp_socket(const char *server_ip, struct sockaddr_in *servaddr) {
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("socket udp");
        return -1;
    }

    memset(servaddr, 0, sizeof(*servaddr));
    servaddr->sin_family = AF_INET;
    servaddr->sin_port = htons(0);

    if (inet_pton(AF_INET, server_ip, &servaddr->sin_addr) <= 0) {
        perror("inet_pton");
        close(udp_sock);
        return -1;
    }

    if (bind(udp_sock, (struct sockaddr*)servaddr, sizeof(*servaddr)) < 0) {
        perror("bind udp");
        close(udp_sock);
        return -1;
    }

    return udp_sock;
}

static int setup_tcp_socket(const struct sockaddr_in *servaddr) {
    int tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) {
        perror("socket tcp");
        return -1;
    }

    if (bind(tcp_sock, (struct sockaddr*)servaddr, sizeof(*servaddr)) < 0) {
        perror("bind tcp");
        close(tcp_sock);
        return -1;
    }

    if (listen(tcp_sock, TCP_BACKLOG) < 0) {
        perror("listen tcp");
        close(tcp_sock);
        return -1;
    }

    return tcp_sock;
}

static void cleanup_resources(FILE *client_files[], int size, int *lostPositions, int *rejectCount, int udp_sock, int tcp_sock) {
    for (int i = 0; i < size; i++) {
        if (client_files[i]) fclose(client_files[i]);
    }
    if (common_tcp_log_file) fclose(common_tcp_log_file);
    free(lostPositions);
    free(rejectCount);
    close(udp_sock);
    close(tcp_sock);
}

static int handle_udp_packet(int udp_sock, FILE *client_files[], int *lostPositions, int lostSize, int *rejectCount) {
    char buffer[MAX_UDP_PACKET_SIZE];
    struct sockaddr_in clientaddr;
    socklen_t len = sizeof(clientaddr);

    ssize_t n = recvfrom(udp_sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&clientaddr, &len);
    if (n < 0) {
        perror("recvfrom");
        return -1;
    }
    buffer[n] = '\0';

    if (n < 1) {
        fprintf(stderr, "Received empty packet\n");
        return 0;
    }

    int client_port = ntohs(clientaddr.sin_port);
    char temp_filename[32];
    snprintf(temp_filename, sizeof(temp_filename), "%d.bin", client_port);

    FILE *f = client_files[client_port];

    unsigned char check1 = (unsigned char)buffer[0];
    unsigned char check2 = (unsigned char)buffer[1];
    unsigned char check3 = (unsigned char)buffer[2];

    if (check1 != 255 || check2 != 255 || check3 != 255) {
        if (f) {
            fclose(f);
            client_files[client_port] = NULL;
        }
        if (rename(temp_filename, buffer) == 0) {
            printf("File for client port %d renamed to %s\n", client_port, buffer);
        } else {
            perror("rename");
        }
        if (sendto(udp_sock, ACK, strlen(ACK), 0, (struct sockaddr *)&clientaddr, len) < 0) {
            perror("sendto");
            return -1;
        }
        return 0;
    }

    if (!f) {
        f = fopen(temp_filename, "wb");
        if (!f) {
            perror("fopen");
            return 0;
        }
        client_files[client_port] = f;
        printf("Opened file %s for client port %d\n", temp_filename, client_port);
    }

    if (n < 8) {
        fprintf(stderr, "Packet too small\n");
        return 0;
    }

    unsigned char data_byte = (unsigned char)buffer[3];
    int packet_num = (unsigned char)buffer[4] 
                    | ((unsigned char)buffer[5] << 8) 
                    | ((unsigned char)buffer[6] << 16) 
                    | ((unsigned char)buffer[7] << 24);

    if (packet_num < lostSize && rejectCount[packet_num] < lostPositions[packet_num]) {
        rejectCount[packet_num]++;
        printf("Rejecting packet number %d (%d/%d)\n", packet_num, rejectCount[packet_num], lostPositions[packet_num]);
        return 0;
    }

    if (fwrite(&data_byte, 1, 1, f) != 1) {
        perror("fwrite");
        return -1;
    }

    if (sendto(udp_sock, ACK, strlen(ACK), 0, (struct sockaddr *)&clientaddr, len) < 0) {
        perror("sendto");
        return -1;
    } else {
        printf("Sent ACK for packet number %d\n", packet_num);
    }

    return 0;
}

static int handle_tcp_connection(int tcp_sock) {
    struct sockaddr_in clientaddr;
    socklen_t len = sizeof(clientaddr);

    int client_fd = accept(tcp_sock, (struct sockaddr*)&clientaddr, &len);
    if (client_fd < 0) {
        perror("accept");
        return -1;
    }

    char client_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &clientaddr.sin_addr, client_ip, sizeof(client_ip)) != NULL) {
        printf("New TCP connection from %s:%d\n", client_ip, ntohs(clientaddr.sin_port));
    }

    int *arg = malloc(sizeof(int));
    if (!arg) {
        perror("malloc");
        close(client_fd);
        return -1;
    }
    *arg = client_fd;

    pthread_t tid;
    if (pthread_create(&tid, NULL, tcp_client_handler, arg) != 0) {
        perror("pthread_create");
        close(client_fd);
        free(arg);
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s server_ip [packet_positions]\nExample: %s 127.0.0.1 [1,5,6]\n", argv[0], argv[0]);
        exit(EXIT_FAILURE);
    }

    int lostSize = 0;
    int *lostPositions = parse_packet_positions(argv[2], &lostSize);
    if (!lostPositions && lostSize != 0) {
        fprintf(stderr, "Failed to parse lost positions\n");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in servaddr;
    int udp_sock = setup_udp_socket(argv[1], &servaddr);
    if (udp_sock < 0) {
        free(lostPositions);
        exit(EXIT_FAILURE);
    }

    socklen_t servLen = sizeof(servaddr);
    if (getsockname(udp_sock, (struct sockaddr*)&servaddr, &servLen) == -1) {
        perror("getsockname");
        close(udp_sock);
        free(lostPositions);
        exit(EXIT_FAILURE);
    }

    int tcp_sock = setup_tcp_socket(&servaddr);
    if (tcp_sock < 0) {
        close(udp_sock);
        free(lostPositions);
        exit(EXIT_FAILURE);
    }

    int server_port = ntohs(servaddr.sin_port);
    printf("Server running on port %d (TCP+UDP)\n", server_port);

    int *rejectCount = calloc(lostSize, sizeof(int));
    if (!rejectCount) {
        perror("calloc");
        cleanup_resources(NULL, 0, lostPositions, NULL, udp_sock, tcp_sock);
        exit(EXIT_FAILURE);
    }

    FILE *client_files[65536] = {NULL};

    common_tcp_log_file = fopen("tcp_messages.log", "a");
    if (!common_tcp_log_file) {
        perror("open tcp_messages.log");
        cleanup_resources(client_files, 65536, lostPositions, rejectCount, udp_sock, tcp_sock);
        exit(EXIT_FAILURE);
    }

    fd_set readfds;
    int maxfd = udp_sock > tcp_sock ? udp_sock : tcp_sock;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(udp_sock, &readfds);
        FD_SET(tcp_sock, &readfds);

        int ret = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (ret < 0) {
            perror("select");
            break;
        }

        if (FD_ISSET(udp_sock, &readfds)) {
            if (handle_udp_packet(udp_sock, client_files, lostPositions, lostSize, rejectCount) < 0) {
                break;
            }
        }

        if (FD_ISSET(tcp_sock, &readfds)) {
            if (handle_tcp_connection(tcp_sock) < 0) {
                fprintf(stderr, "Failed to handle tcp connection\n");
            }
        }
    }

    cleanup_resources(client_files, 65536, lostPositions, rejectCount, udp_sock, tcp_sock);
    return 0;
}
