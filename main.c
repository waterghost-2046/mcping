#include <sys/types.h>
#ifdef _WIN32
#include <Winsock2.h>
#include <Ws2tcpip.h>
#else
#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define HANDSHAKE_SIZE 4096
#define STRING_BUF_SIZE 4096
#define PROTOCOL_VERSION 769
#define TIMEOUT_USEC 500000

#ifdef _WIN32
#pragma comment(lib, "Ws2_32.lib")
typedef SSIZE_T ssize_t;
#endif

size_t write_varint(unsigned char *buffer, int value) {
    size_t i = 0;
    do {
        unsigned char temp = value & 0x7F;
        value >>= 7;
        if (value != 0) {
            temp |= 0x80;
        }
        buffer[i++] = temp;
    } while (value != 0);
    return i;
}

int connect_with_timeout(struct addrinfo *addr, suseconds_t timeout_usec) {
    int sockfd;
    int res;

    sockfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    res = connect(sockfd, addr->ai_addr, addr->ai_addrlen);
    if (res < 0 && errno != EINPROGRESS) {
        perror("Connection failed");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int set_socket_timeout(int sockfd, suseconds_t timeout_usec) {
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = timeout_usec;

    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("Set socket timeout failed");
        return -1;
    }

    return 0;
}

size_t build_handshake(unsigned char *buffer, const char *host, unsigned short port) {
    size_t host_len = strlen(host);
    unsigned char packet_body[HANDSHAKE_SIZE];
    size_t pos = 0;

    pos += write_varint(packet_body + pos, 0);
    pos += write_varint(packet_body + pos, PROTOCOL_VERSION);
    pos += write_varint(packet_body + pos, host_len);
    memcpy(packet_body + pos, host, host_len);
    pos += host_len;
    packet_body[pos++] = (port >> 8) & 0xFF;
    packet_body[pos++] = port & 0xFF;
    pos += write_varint(packet_body + pos, 1);

    size_t length_size = write_varint(buffer, pos);
    memcpy(buffer + length_size, packet_body, pos);

    return length_size + pos;
}

int read_varint(int sockfd) {
    int numread = 0;
    int result = 0;
    char byte;

    do {
        ssize_t n = recv(sockfd, &byte, 1, 0);
        if (n == 0) {
            fprintf(stderr, "Failed to read varint: EOF\n");
            exit(EXIT_FAILURE);
        }
        int value = byte & 0x7F;
        result |= value << (7 * numread);

        numread++;

        if (numread > 5) {
            fprintf(stderr, "Varint too big\n");
            exit(EXIT_FAILURE);
        }
    } while ((byte & 0x80) != 0);

    return result;
}

int main(int argc, char **argv) {
    unsigned short port = 25565;
    struct addrinfo hints, *result, *rp;
    int sockfd, res;
    ssize_t nread;
    size_t handshake_len;
    unsigned char handshake[HANDSHAKE_SIZE];
    char request[] = {0x01, 0x00};
    char response[STRING_BUF_SIZE];

    if (argc < 2 || strlen(argv[1]) > 250) {
        fprintf(stderr, "Usage: %s <host> [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 3) {
        port = atoi(argv[2]);
    }

    if (port == 0) {
        fprintf(stderr, "Invalid port\n");
        return EXIT_FAILURE;
    }

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);
    res = getaddrinfo(argv[1], port_str, &hints, &result);
    if (res != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(res));
        return EXIT_FAILURE;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sockfd = connect_with_timeout(rp, TIMEOUT_USEC);
        if (sockfd != -1) {
            break;
        }
    }

    if (rp == NULL) {
        fprintf(stderr, "Could not connect\n");
        freeaddrinfo(result);
        return EXIT_FAILURE;
    }

    if (set_socket_timeout(sockfd, TIMEOUT_USEC) == -1) {
        close(sockfd);
        freeaddrinfo(result);
        return EXIT_FAILURE;
    }

    freeaddrinfo(result);

    handshake_len = build_handshake(handshake, argv[1], port);
    if (send(sockfd, handshake, handshake_len, 0) != handshake_len) {
        perror("Failed to send handshake");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (send(sockfd, request, sizeof(request), 0) != sizeof(request)) {
        perror("Failed to send request");
        close(sockfd);
        return EXIT_FAILURE;
    }

    read_varint(sockfd);
    char packet_id;
    if (recv(sockfd, &packet_id, 1, 0) == 0) {
        fprintf(stderr, "Failed to read packet id\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (packet_id != 0) {
        fprintf(stderr, "Unexpected packet id\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    int json_len = read_varint(sockfd);
    while (json_len > 0) {
        nread = recv(sockfd, response, STRING_BUF_SIZE, 0);
        if (nread <= 0) {
            perror("Failed to read JSON response");
            close(sockfd);
            return EXIT_FAILURE;
        }
        json_len -= nread;
        fwrite(response, 1, nread, stdout);
    }

    close(sockfd);

    return EXIT_SUCCESS;
}
