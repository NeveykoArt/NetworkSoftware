#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>

static const char *ACK = "ACK";

static void fail(const char *msg, FILE *file, int sockfd) {
    perror(msg);
    if (file) fclose(file);
    if (sockfd >= 0) close(sockfd);
    exit(EXIT_FAILURE);
}

typedef struct {
    unsigned char check1;
    unsigned char check2;
    unsigned char check3;
    unsigned char data;
    int packet_number;
} buffer;

static void send_with_ack(int sockfd, struct sockaddr_in *servaddr, socklen_t addr_len, const char* data, size_t data_len, const char *desc) {
    char recv_buffer[16];
    while (1) {
        ssize_t sent = sendto(sockfd, data, data_len, 0, (struct sockaddr *)servaddr, addr_len);
        if (sent != (ssize_t)data_len) {
            fail("sendto", NULL, sockfd);
        }

        ssize_t recvd = recvfrom(sockfd, recv_buffer, sizeof(recv_buffer) - 1, 0, NULL, NULL);
        if (recvd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("Timeout waiting for ACK of %s, resending\n", desc);
                continue;
            } else {
                fail("recvfrom", NULL, sockfd);
            }
        }

        recv_buffer[recvd] = '\0';

        if (strcmp(recv_buffer, ACK) == 0) {
            printf("Received ACK for %s\n", desc);
            break;
        } else {
            printf("Received unexpected response: %s, resending %s\n", recv_buffer, desc);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s server_ip server_port filename\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    const char *filename = argv[3];

    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        fclose(file);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in servaddr = {0};
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_ip, &servaddr.sin_addr) <= 0) {
        fail("inet_pton", file, sockfd);
    }

    struct timeval timeout = {3, 0};
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        fail("setsockopt", file, sockfd);
    }

    socklen_t addr_len = sizeof(servaddr);
    buffer send_buffer;
    int packet_num = 0;

    while (1) {
        int c = fgetc(file);
        if (c == EOF) {
            printf("File sent successfully\n");
            break;
        }

        send_buffer.packet_number = packet_num;
        send_buffer.data = c;
        send_buffer.check1 = 255;
        send_buffer.check2 = 255;
        send_buffer.check3 = 255;

        char bytes[32];
        snprintf(bytes, sizeof(bytes), "packet %d", packet_num);
        send_with_ack(sockfd, &servaddr, addr_len, (char *)&send_buffer, sizeof(send_buffer), bytes);

        packet_num++;
    }
    send_with_ack(sockfd, &servaddr, addr_len, filename, strlen(filename), "filename");

    fclose(file);
    close(sockfd);
    exit(EXIT_SUCCESS);
}
