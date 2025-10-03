#include <sys/types.h>
#ifdef _WIN32
#include <Winsock2.h>
#include <Ws2tcpip.h>
#else
#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define HANDSHAKE_SIZE 4096
#define STRING_BUF_SIZE 4096
#define PROTOCOL_VERSION 769
#define TIMEOUT_SEC 5
#define TIMEOUT_USEC 0

#ifdef _WIN32
#pragma comment(lib, "Ws2_32.lib")
typedef SSIZE_T ssize_t;
#define close closesocket

int init_winsock() {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData);
}
#else
int init_winsock() { return 0; }
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

int connect_with_timeout(struct addrinfo *addr, int timeout_sec) {
    int sockfd;
    int res;
    struct timeval timeout;
    fd_set fdset;

    sockfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return -1;
    }

#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(sockfd, FIONBIO, &mode) != 0) {
        perror("Failed to set non-blocking mode");
        close(sockfd);
        return -1;
    }
#else
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0 || fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("Failed to set non-blocking mode");
        close(sockfd);
        return -1;
    }
#endif

    res = connect(sockfd, addr->ai_addr, addr->ai_addrlen);
    
#ifdef _WIN32
    if (res == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
#else
    if (res < 0 && errno != EINPROGRESS) {
#endif
        perror("Connection failed immediately");
        close(sockfd);
        return -1;
    }

    if (res == 0) {
        // 즉시 연결 성공
#ifdef _WIN32
        mode = 0;
        ioctlsocket(sockfd, FIONBIO, &mode);
#else
        fcntl(sockfd, F_SETFL, flags);
#endif
        return sockfd;
    }

    FD_ZERO(&fdset);
    FD_SET(sockfd, &fdset);
    timeout.tv_sec = timeout_sec;
    timeout.tv_usec = 0;

    res = select(sockfd + 1, NULL, &fdset, NULL, &timeout);
    if (res <= 0) {
        if (res == 0) {
            fprintf(stderr, "Connection timeout\n");
        } else {
            perror("select() error");
        }
        close(sockfd);
        return -1;
    }

    int optval;
    socklen_t optlen = sizeof(optval);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char*)&optval, &optlen) < 0 || optval != 0) {
        fprintf(stderr, "Connection error: %s\n", strerror(optval));
        close(sockfd);
        return -1;
    }

#ifdef _WIN32
    mode = 0;
    ioctlsocket(sockfd, FIONBIO, &mode); // 블로킹 모드로 (윈도우)
#else
    fcntl(sockfd, F_SETFL, flags); // 블로킹 모드로
#endif

    return sockfd;
}

int set_socket_timeout(int sockfd, int timeout_sec) {
#ifdef _WIN32
    DWORD timeout = timeout_sec * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout)) < 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout)) < 0) {
#else
    struct timeval timeout;
    timeout.tv_sec = timeout_sec;
    timeout.tv_usec = 0;

    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
#endif
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
    unsigned char byte;

    do {
        ssize_t n = recv(sockfd, (char*)&byte, 1, 0);
        if (n <= 0) {
            if (n == 0) {
                fprintf(stderr, "Connection closed while reading varint\n");
            } else {
                perror("Failed to read varint");
            }
            return -1;
        }
        int value = byte & 0x7F;
        result |= value << (7 * numread);
        numread++;
        if (numread > 5) {
            fprintf(stderr, "Varint too big\n");
            return -1;
        }
    } while ((byte & 0x80) != 0);

    return result;
}

int socks5_connect(const char *proxy_host, unsigned short proxy_port, 
                   const char *dest_host, unsigned short dest_port, int timeout_sec) {
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

    sockfd = connect_with_timeout(res, timeout_sec);
    freeaddrinfo(res);
    if (sockfd < 0) return -1;

    if (set_socket_timeout(sockfd, timeout_sec) == -1) {
        close(sockfd);
        return -1;
    }

    unsigned char req1[] = {0x05, 0x01, 0x00};
    if (send(sockfd, (char*)req1, sizeof(req1), 0) != sizeof(req1)) {
        perror("SOCKS5 handshake send failed");
        close(sockfd);
        return -1;
    }

    unsigned char resp1[2];
    if (recv(sockfd, (char*)resp1, 2, 0) != 2 || resp1[1] != 0x00) {
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
    req2[pos++] = (unsigned char)host_len;
    memcpy(req2 + pos, dest_host, host_len);
    pos += host_len;
    req2[pos++] = (dest_port >> 8) & 0xFF;
    req2[pos++] = dest_port & 0xFF;

    if (send(sockfd, (char*)req2, pos, 0) != (ssize_t)pos) {
        perror("SOCKS5 connect request failed");
        close(sockfd);
        return -1;
    }

    unsigned char resp2[10];
    if (recv(sockfd, (char*)resp2, 10, 0) < 5 || resp2[1] != 0x00) {
        fprintf(stderr, "SOCKS5 connect response failed\n");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

int send_proxy_header(int sockfd, unsigned short server_port) {
    const char *client_ip = "127.0.0.1";
    const char *server_ip = "127.0.0.1";
    srand((unsigned int)time(NULL));
    unsigned short client_port = (rand() % 65534) + 1;

    char header[108];
    int len = snprintf(header, sizeof(header),
        "PROXY TCP4 %s %s %u %u\r\n",
        client_ip, server_ip, client_port, server_port);

    if (send(sockfd, header, len, 0) != len) {
        perror("Failed to send PROXY header");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    unsigned short port = 25565;
    unsigned short socks_port = 0;
    char *socks_host = NULL;
    const char *target_host = NULL;
    int use_ha_protocol = 0;

    if (init_winsock() != 0) {
        fprintf(stderr, "Failed to initialize Winsock\n");
        return EXIT_FAILURE;
    }

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <host> [port] [--socks ip:port] [--ha-protocol]\n", argv[0]);
        return EXIT_FAILURE;
    }

    target_host = argv[1];
    if (argc >= 3 && strncmp(argv[2], "--", 2) != 0) {
        port = (unsigned short)atoi(argv[2]);
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socks") == 0 && i + 1 < argc) {
            char *sep = strchr(argv[i + 1], ':');
            if (sep) {
                *sep = '\0';
                socks_host = argv[i + 1];
                socks_port = (unsigned short)atoi(sep + 1);
            }
        } else if (strcmp(argv[i], "--ha-protocol") == 0) {
            use_ha_protocol = 1;
        }
    }

    int sockfd = -1;
    if (socks_host) {
        sockfd = socks5_connect(socks_host, socks_port, target_host, port, TIMEOUT_SEC);
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
            sockfd = connect_with_timeout(rp, TIMEOUT_SEC);
            if (sockfd != -1) break;
        }

        freeaddrinfo(result);
    }

    if (sockfd < 0) {
        fprintf(stderr, "Connection failed\n");
        return EXIT_FAILURE;
    }

    if (set_socket_timeout(sockfd, TIMEOUT_SEC) == -1) {
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (use_ha_protocol) {
        if (send_proxy_header(sockfd, port) == -1) {
            close(sockfd);
            return EXIT_FAILURE;
        }
    }

    unsigned char handshake[HANDSHAKE_SIZE];
    size_t handshake_len = build_handshake(handshake, target_host, port);
    if (send(sockfd, (char*)handshake, handshake_len, 0) != (ssize_t)handshake_len) {
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

    int packet_len = read_varint(sockfd);
    if (packet_len < 0) {
        fprintf(stderr, "Failed to read packet length\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    char packet_id;
    if (recv(sockfd, &packet_id, 1, 0) <= 0 || packet_id != 0x00) {
        fprintf(stderr, "Unexpected packet id or error\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    int json_len = read_varint(sockfd);
    if (json_len < 0) {
        fprintf(stderr, "Failed to read JSON length\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    char response[STRING_BUF_SIZE];
    ssize_t nread;
    int total_read = 0;

    while (json_len > 0) {
        int to_read = (json_len < STRING_BUF_SIZE) ? json_len : STRING_BUF_SIZE;
        nread = recv(sockfd, response, to_read, 0);
        if (nread <= 0) {
            perror("Failed to read JSON response");
            close(sockfd);
            return EXIT_FAILURE;
        }
        fwrite(response, 1, nread, stdout);
        json_len -= nread;
        total_read += nread;
    }

    close(sockfd);
#ifdef _WIN32
    WSACleanup();
#endif
    return EXIT_SUCCESS;
}
