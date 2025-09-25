#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

static const char *ACK = "ACK";

static void fail(const char *msg, int sockfd) {
    perror(msg);
    if (sockfd >= 0) close(sockfd);
    exit(EXIT_FAILURE);
}

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

typedef struct {
    int packet_number;
    char data;
} buffer;

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s server_ip [packet_positions]\nExample: %s 127.0.0.1 [1,5,6]\n", argv[0], argv[0]);
        exit(EXIT_FAILURE);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in servaddr = {0};
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(0);
    if (inet_pton(AF_INET, argv[1], &servaddr.sin_addr) <= 0) {
        fail("inet_pton", sockfd);
    }

    int lostSize = 0;
    int *lostPositions = parse_packet_positions(argv[2], &lostSize);
    if (!lostPositions && lostSize != 0) {
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        free(lostPositions);
        fail("bind", sockfd);
    }

    socklen_t servLen = sizeof(servaddr);
    if (getsockname(sockfd, (struct sockaddr *)&servaddr, &servLen) == -1) {
        free(lostPositions);
        fail("getsockname", sockfd);
    }

    printf("Server is running on port %d\n", ntohs(servaddr.sin_port));

    int *rejectCount = calloc(lostSize, sizeof(int));
    if (!rejectCount) {
        fail("calloc", sockfd);
        free(lostPositions);
    }

    FILE *client_files[65536] = {NULL};

    while (1) {
        char buffer[1024];
        struct sockaddr_in clientaddr;
        socklen_t len = sizeof(clientaddr);

        ssize_t n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&clientaddr, &len);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }
        buffer[n] = '\0';

        if (n < 1) {
            fprintf(stderr, "Received empty packet\n");
            continue;
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
            if (sendto(sockfd, ACK, strlen(ACK), 0, (struct sockaddr *)&clientaddr, len) < 0) {
                perror("sendto");
                for (int i = 0; i < 65536; i++) {
                        if (client_files[i]) fclose(client_files[i]);
                }
                free(lostPositions);
                free(rejectCount);
                close(sockfd);
                exit(EXIT_FAILURE);
            }
            continue;
        }

        if (!f) {
            f = fopen(temp_filename, "wb");
            if (!f) {
                perror("fopen");
                continue;
            }
            client_files[client_port] = f;
            printf("Opened file %s for client port %d\n", temp_filename, client_port);
        }

        if (n < 8) {
            fprintf(stderr, "Packet too small\n");
            continue;
        }

        unsigned char data_byte = (unsigned char)buffer[3];
        int packet_num = (unsigned char)buffer[4] | ((unsigned char)buffer[5] << 8) | ((unsigned char)buffer[6] << 16) | ((unsigned char)buffer[7] << 24);

        if (packet_num < lostSize && rejectCount[packet_num] < lostPositions[packet_num]) {
            rejectCount[packet_num]++;
            printf("Rejecting packet number %d (%d/%d)\n", packet_num, rejectCount[packet_num], lostPositions[packet_num]);
            continue;
        }

        if (fwrite(&data_byte, 1, 1, f) != 1) {
            perror("fwrite");
            for (int i = 0; i < 65536; i++) {
                if (client_files[i]) fclose(client_files[i]);
            }
            free(lostPositions);
            free(rejectCount);
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        if (sendto(sockfd, ACK, strlen(ACK), 0, (struct sockaddr *)&clientaddr, len) < 0) {
            perror("sendto");
            for (int i = 0; i < 65536; i++) {
                if (client_files[i]) fclose(client_files[i]);
            }
            free(lostPositions);
            free(rejectCount);
            close(sockfd);
            exit(EXIT_FAILURE);
        } else {
            printf("Sent ACK for packet number %d\n", packet_num);
        }
    }

    for (int i = 0; i < 65536; i++) {
        if (client_files[i]) fclose(client_files[i]);
    }

    free(lostPositions);
    free(rejectCount);
    close(sockfd);

    exit(EXIT_SUCCESS);
}