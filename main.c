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
    int flags, res;
    struct timeval timeout;
    fd_set fdset;

    sockfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return -1;
    }

    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("Failed to set non-blocking mode");
        close(sockfd);
        return -1;
    }

    res = connect(sockfd, addr->ai_addr, addr->ai_addrlen);
    if (res == 0) {
        fcntl(sockfd, F_SETFL, flags);
        return sockfd;
    } else if (errno != EINPROGRESS) {
        perror("Connection failed immediately");
        close(sockfd);
        return -1;
    }

    FD_ZERO(&fdset);
    FD_SET(sockfd, &fdset);
    timeout.tv_sec = timeout_usec / 1000000;
    timeout.tv_usec = timeout_usec % 1000000;

    res = select(sockfd + 1, NULL, &fdset, NULL, &timeout);
    if (res <= 0) {
        perror("Connection timeout or error");
        close(sockfd);
        return -1;
    }

    int optval;
    socklen_t optlen = sizeof(optval);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0 || optval != 0) {
        perror("Connection error after select()");
        close(sockfd);
        return -1;
    }

    fcntl(sockfd, F_SETFL, flags);
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
        if (n <= 0) {
            fprintf(stderr, "Failed to read varint: EOF or error\n");
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

int socks5_connect(const char *proxy_host, unsigned short proxy_port, const char *dest_host, unsigned short dest_port) {
    struct addrinfo hints, *res;
    int sockfd;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", proxy_port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(proxy_host, port_str, &hints, &res) != 0) {
        perror("getaddrinfo for SOCKS proxy failed");
        return -1;
    }

    sockfd = connect_with_timeout(res, TIMEOUT_USEC);
    freeaddrinfo(res);
    if (sockfd < 0) return -1;

    unsigned char req1[] = {0x05, 0x01, 0x00};
    if (send(sockfd, req1, sizeof(req1), 0) != sizeof(req1)) {
        perror("SOCKS5 handshake send failed");
        close(sockfd);
        return -1;
    }

    unsigned char resp1[2];
    if (recv(sockfd, resp1, 2, 0) != 2 || resp1[1] != 0x00) {
        fprintf(stderr, "SOCKS5 handshake failed\n");
        close(sockfd);
        return -1;
    }

    size_t host_len = strlen(dest_host);
    unsigned char req2[300];
    size_t pos = 0;
    req2[pos++] = 0x05;
    req2[pos++] = 0x01;
    req2[pos++] = 0x00;
    req2[pos++] = 0x03;
    req2[pos++] = host_len;
    memcpy(req2 + pos, dest_host, host_len);
    pos += host_len;
    req2[pos++] = (dest_port >> 8) & 0xFF;
    req2[pos++] = dest_port & 0xFF;

    if (send(sockfd, req2, pos, 0) != pos) {
        perror("SOCKS5 connect request failed");
        close(sockfd);
        return -1;
    }

    unsigned char resp2[10];
    if (recv(sockfd, resp2, 10, 0) < 5 || resp2[1] != 0x00) {
        fprintf(stderr, "SOCKS5 connect response failed\n");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int main(int argc, char **argv) {
    unsigned short port = 25565;
    unsigned short socks_port = 0;
    char *socks_host = NULL;
    const char *target_host = NULL;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <host> [port] [--socks ip:port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    target_host = argv[1];
    if (argc >= 3 && strncmp(argv[2], "--", 2) != 0) {
        port = atoi(argv[2]);
    }

    for (int i = 1; i < argc - 1; ++i) {
        if (strcmp(argv[i], "--socks") == 0) {
            char *sep = strchr(argv[i + 1], ':');
            if (sep) {
                *sep = '\0';
                socks_host = argv[i + 1];
                socks_port = atoi(sep + 1);
            }
        }
    }

    int sockfd = -1;
    if (socks_host) {
        sockfd = socks5_connect(socks_host, socks_port, target_host, port);
    } else {
        struct addrinfo hints, *result, *rp;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        char port_str[6];
        snprintf(port_str, sizeof(port_str), "%d", port);
        if (getaddrinfo(target_host, port_str, &hints, &result) != 0) {
            perror("getaddrinfo");
            return EXIT_FAILURE;
        }

        for (rp = result; rp != NULL; rp = rp->ai_next) {
            sockfd = connect_with_timeout(rp, TIMEOUT_USEC);
            if (sockfd != -1) break;
        }

        freeaddrinfo(result);
    }

    if (sockfd < 0) {
        fprintf(stderr, "Connection failed\n");
        return EXIT_FAILURE;
    }

    if (set_socket_timeout(sockfd, TIMEOUT_USEC) == -1) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    unsigned char handshake[HANDSHAKE_SIZE];
    size_t handshake_len = build_handshake(handshake, target_host, port);
    if (send(sockfd, handshake, handshake_len, 0) != handshake_len) {
        perror("Failed to send handshake");
        close(sockfd);
        return EXIT_FAILURE;
    }

    char request[] = {0x01, 0x00};
    if (send(sockfd, request, sizeof(request), 0) != sizeof(request)) {
        perror("Failed to send request");
        close(sockfd);
        return EXIT_FAILURE;
    }

    read_varint(sockfd);
    char packet_id;
    if (recv(sockfd, &packet_id, 1, 0) <= 0 || packet_id != 0x00) {
        fprintf(stderr, "Unexpected packet id or error\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    int json_len = read_varint(sockfd);
    char response[STRING_BUF_SIZE];
    ssize_t nread;

    while (json_len > 0) {
        nread = recv(sockfd, response, STRING_BUF_SIZE, 0);
        if (nread <= 0) {
            perror("Failed to read JSON response");
            close(sockfd);
            return EXIT_FAILURE;
        }
        fwrite(response, 1, nread, stdout);
        json_len -= nread;
    }

    close(sockfd);
    return EXIT_SUCCESS;
}
