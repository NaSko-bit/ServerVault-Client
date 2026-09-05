#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_SERVER_IP "127.0.0.1"
#define PORT 2000
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int send_all(int client_fd, const void* data, size_t length)
{
    const char* bytes = data;
    size_t sent_total = 0;

    while (sent_total < length) {
        ssize_t sent = send(client_fd, bytes + sent_total,
                            length - sent_total, 0);
        if (sent <= 0) {
            return -1;
        }
        sent_total += (size_t)sent;
    }

    return 0;
}

static int receive_exact(int client_fd, char* response, size_t length)
{
    size_t received_total = 0;

    while (received_total < length) {
        ssize_t received = read(client_fd, response + received_total,
                                length - received_total);
        if (received <= 0) {
            return -1;
        }
        received_total += (size_t)received;
    }

    return 0;
}

static int expect_response(int client_fd, const char* expected)
{
    size_t length = strlen(expected);
    char response[128];

    if (length >= sizeof(response)
        || receive_exact(client_fd, response, length) < 0
        || memcmp(response, expected, length) != 0) {
        return -1;
    }

    return 0;
}

static int send_file_bytes(int client_fd, FILE* fp)
{
    char data[1024];
    size_t bytes_read;

    while ((bytes_read = fread(data, 1, sizeof(data), fp)) > 0) {
        if (send_all(client_fd, data, bytes_read) < 0) {
            return -1;
        }
    }

    return ferror(fp) ? -1 : 0;
}

int file_transfer(int client_fd, const char* filename)
{
    FILE* fp = fopen(filename, "rb");
    if (fp == NULL) {
        perror("File open error");
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long file_size = ftell(fp);
    if (file_size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    char header[1024];
    int header_length = snprintf(header, sizeof(header),
                                 "UPLOAD %s %ld\n", filename, file_size);
    if (header_length < 0 || (size_t)header_length >= sizeof(header)
        || send_all(client_fd, header, (size_t)header_length) < 0) {
        fclose(fp);
        return -1;
    }

    const char ready_response[] = "READY\n";
    char response[sizeof(ready_response)];
    if (receive_exact(client_fd, response, sizeof(ready_response) - 1) < 0) {
        fclose(fp);
        return -1;
    }
    if (memcmp(response, ready_response, sizeof(ready_response) - 1) != 0) {
        printf("Unexpected server response: %.*s",
               (int)(sizeof(ready_response) - 1), response);
        fclose(fp);
        return -1;
    }

    int read_error = send_file_bytes(client_fd, fp);
    fclose(fp);
    if (read_error) {
        return -1;
    }

    const char complete_response[] = "UPLOAD_COMPLETE";
    char complete_buffer[sizeof(complete_response)];
    if (receive_exact(client_fd, complete_buffer,
                      sizeof(complete_response) - 1) < 0) {
        return -1;
    }
    if (memcmp(complete_buffer, complete_response,
               sizeof(complete_response) - 1) != 0) {
        printf("Unexpected server response: %.*s",
               (int)(sizeof(complete_response) - 1), complete_buffer);
        return -1;
    }

    char terminator;
    recv(client_fd, &terminator, 1, MSG_DONTWAIT);

    return 0;
}

static int sync_client_memory(int client_fd)
{
    DIR* directory = opendir("./ClientMemory");
    if (directory == NULL) {
        perror("ClientMemory");
        return -1;
    }

    size_t file_count = 0;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        char path[PATH_MAX];
        struct stat file_info;
        int path_length = snprintf(path, sizeof(path),
                                   "./ClientMemory/%s", entry->d_name);
        if (path_length < 0 || (size_t)path_length >= sizeof(path)
            || stat(path, &file_info) != 0) {
            continue;
        }
        if (S_ISREG(file_info.st_mode)) {
            file_count++;
        }
    }
    closedir(directory);

    char header[128];
    int header_length = snprintf(header, sizeof(header), "SYNC %zu\n",
                                 file_count);
    if (header_length < 0 || (size_t)header_length >= sizeof(header)
        || send_all(client_fd, header, (size_t)header_length) < 0
        || expect_response(client_fd, "SYNC_READY\n") < 0) {
        return -1;
    }

    directory = opendir("./ClientMemory");
    if (directory == NULL) {
        return -1;
    }

    while ((entry = readdir(directory)) != NULL) {
        char path[PATH_MAX];
        struct stat file_info;
        int path_length = snprintf(path, sizeof(path),
                                   "./ClientMemory/%s", entry->d_name);
        if (path_length < 0 || (size_t)path_length >= sizeof(path)
            || stat(path, &file_info) != 0 || !S_ISREG(file_info.st_mode)) {
            continue;
        }

        FILE* fp = fopen(path, "rb");
        if (fp == NULL) {
            closedir(directory);
            return -1;
        }

        header_length = snprintf(header, sizeof(header), "FILE %s %lld\n",
                                 entry->d_name,
                                 (long long)file_info.st_size);
        if (header_length < 0 || (size_t)header_length >= sizeof(header)
            || send_all(client_fd, header, (size_t)header_length) < 0
            || expect_response(client_fd, "READY\n") < 0
            || send_file_bytes(client_fd, fp) < 0) {
            fclose(fp);
            closedir(directory);
            return -1;
        }
        fclose(fp);

        if (expect_response(client_fd, "FILE_STORED\n") < 0) {
            closedir(directory);
            return -1;
        }
        if (unlink(path) != 0) {
            perror("Could not remove synchronized file from ClientMemory");
            closedir(directory);
            return -1;
        }
    }
    closedir(directory);

    if (send_all(client_fd, "SYNC_DONE\n", strlen("SYNC_DONE\n")) < 0
        || expect_response(client_fd, "SYNC_COMPLETE\n") < 0) {
        return -1;
    }

    return 0;
}

void server_status(int status, const char* server_ip)
{
    if (status < 0) {
        printf("\nHOST: OFFLINE \n");
        printf("Files remain safely stored in ClientMemory\n");
    }
    else {
        printf("\nHOST: ONLINE \n");
        printf("Address: %s:%d\n", server_ip, PORT);
    }
}

int main(int argc, char const* argv[])
{
    int status, valread, client_fd;
    const char* server_ip = argc > 1 ? argv[1] : DEFAULT_SERVER_IP;
    struct sockaddr_in serv_addr;
    char buffer[1024] = { 0 };
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // Convert IPv4 and IPv6 addresses from text to binary
    // form
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr)
        <= 0) {
        printf(
            "\nInvalid address/ Address not supported \n");
        return -1;
    }

    if ((status
         = connect(client_fd, (struct sockaddr*)&serv_addr,
                   sizeof(serv_addr)))
        < 0) {
        fprintf(stderr, "Connection to %s:%d failed: %s\n",
                server_ip, PORT, strerror(errno));
        server_status(-1, server_ip);
        return -1;
    }
    server_status(1, server_ip);

    while (1) {
        printf("Enter message to send to server: ");
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            break;
        }
        if (strcmp(buffer, "exit\n") == 0 || strcmp(buffer, "quit\n") == 0) {
            break;
        }

        if (strncmp(buffer, "upload ", 7) == 0) {
            char* filename = buffer + 7;
            filename[strcspn(filename, "\n")] = '\0';
            if (file_transfer(client_fd, filename) < 0) {
                printf("File upload failed\n");
                continue;
            }
            if (strncmp(filename, "./ClientMemory/", 15) == 0
                && unlink(filename) != 0) {
                perror("Could not remove uploaded file from ClientMemory");
                continue;
            }
            printf("File uploaded: %s\n", filename);
            continue;
        }
        if (strcmp(buffer, "sync\n") == 0) {
            if (sync_client_memory(client_fd) < 0) {
                printf("Synchronization failed\n");
                continue;
            }
            printf("ClientMemory synchronized\n");
            continue;
        }
        else if (send(client_fd, buffer, strlen(buffer), 0) == -1) {
            perror("Error sending command");
            break;
        }
        printf("Message sent\n");
        valread = read(client_fd, buffer, 1024 - 1);
        if (valread <= 0) {
            printf("Server disconnected\n");
            break;
        }
        buffer[valread] = '\0';
        printf("Server response: %s", buffer);
    }

    // closing the connected socket
    close(client_fd);
    return 0;
}

