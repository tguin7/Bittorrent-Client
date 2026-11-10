// Include all the linux functions needed for this program
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <openssl/evp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Limit the replies to 1 MiB and only give 5 seconds to exchange the info
#define LIMIT (1024 * 1024)
#define TIMEOUT_MS 5000

// Structure to hold the tracker configuration, which is made up of tracker address,
// port and refresh interval
typedef struct
{
    char host[256], port[6];
    unsigned long interval;
} Config;

// Show an error message
static void error(const char *s)
{
    fprintf(stderr, "Error: %s\n", s);
}

// Check and see if the input is a whole number that is zero or positive
static int number(const char *s, unsigned long long *n)
{
    char *end;

    if (!*s)
    {
        return 0;
    }

    for (const char *p = s; *p; ++p)
    {
        if (!isdigit((unsigned char)*p))
        {
            return 0;
        }
    }

    errno = 0;

    *n = strtoull(s, &end, 10);

    return !errno && !*end;
}

// Check the length of the filename and the number of characters that are allowed
static int filename(const char *s)
{
    size_t n = strlen(s);

    if (!n || n > 180 || s[0] == '.')
    {
        return 0;
    }

    // Allow letters, digits, dots, underscores and hyphens
    for (; *s; ++s)
    {
        if (!isalnum((unsigned char)*s) && !strchr("._-", *s))
        {
            return 0;
        }
    }

    return 1;
}

// Check and see if the MD5 checksum has the expected format
static int digest_text(const char *s)
{
    if (strlen(s) != 32)
    {
        return 0;
    }

    // A hexadecimal character is labeled as a letter from A to F or as a digit
    for (; *s; ++s)
    {
        if (!isxdigit((unsigned char)*s))
        {
            return 0;
        }
    }

    return 1;
}

// Calculate the MD5 checksum
static int md5(const void *data, size_t len, char out[33])
{
    unsigned char bytes[EVP_MAX_MD_SIZE];
    unsigned int n = 0;

    // OpenSSL calculates the checksum as 16 bytes
    if (!EVP_Digest(data, len, bytes, &n, EVP_md5(), NULL) || n != 16)
    {
        return 0;
    }

    // Write the checksum as a hexadecimal number that has 32 characters and is readable
    for (unsigned int i = 0; i < n; ++i)
    {
        snprintf(out + 2 * i, 3, "%02x", bytes[i]);
    }

    return 1;
}

// Read the tracker settings from the configuration file
static int load_cfg(const char *path, Config *c)
{
    FILE *f = fopen(path, "r");
    char line[300];
    unsigned long long val;

    if (!f)
    {
        perror(path);
        return 0;
    }

    // The configuration must contain three settings.
    for (int i = 0; i < 3; ++i)
    {
        if (!fgets(line, sizeof line, f))
        {
            goto bad;
        }

        // Remove the line ending.
        line[strcspn(line, "\r\n")] = 0;

        if (i == 0)
        {
            // Read and check the tracker port.
            if (!number(line, &val) || val < 1 || val > 65535)
            {
                goto bad;
            }

            snprintf(c->port, sizeof c->port, "%llu", val);
        }
        else if (i == 1)
        {
            // Take a valid IPv4 or IPv6 tracker address.
            unsigned char addr[16];

            if (inet_pton(AF_INET, line, addr) != 1 &&
                inet_pton(AF_INET6, line, addr) != 1)
            {
                goto bad;
            }

            strcpy(c->host, line);
        }
        else
        {
            // Read and store the update interval in seconds.
            if (!number(line, &val) || !val || val > ULONG_MAX)
            {
                goto bad;
            }

            c->interval = (unsigned long)val;
        }
    }

    // Only whitespace is allowed after the three settings.
    while (fgets(line, sizeof line, f))
    {
        for (char *p = line; *p; ++p)
        {
            if (!isspace((unsigned char)*p))
            {
                goto bad;
            }
        }
    }

    if (ferror(f))
    {
        goto bad;
    }

    fclose(f);

    return 1;

bad:
    // Report an invalid or incomplete configuration.
    fclose(f);
    error("config must contain port, numeric tracker IP, refresh seconds on three lines");
    return 0;
}

// Make a timer that tracks how much time was taken in milliseconds
static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);

    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Check for the network activity that is in the allowed time
static int ready(int fd, short events, long long deadline)
{
    struct pollfd p = {fd, events, 0};
    int rc;

    for (;;)
    {
        long long left = deadline - now_ms();

        if (left <= 0)
        {
            error("network deadline exceeded");
            return 0;
        }

        rc = poll(&p, 1, (int)left);

        // Try again if the signal got interrupted
        if (rc < 0 && errno == EINTR)
        {
            continue;
        }

        if (rc <= 0)
        {
            error("network timeout or poll error");
            return 0;
        }

        // Continue to the next network operation
        return 1;
    }
}

// Connect to the tracker, send in a request and get the reply
static char *talk(const Config *c, const char *request, size_t *size)
{
    struct addrinfo hints = {0}, *ai = NULL;
    int fd = -1;
    char *buf = NULL;

    long long deadline = now_ms() + TIMEOUT_MS;
    size_t sent = 0, used = 0;

    // Set the tracker address to take in TCP connection
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

    if (getaddrinfo(c->host, c->port, &hints, &ai))
    {
        error("invalid tracker address");
        return NULL;
    }

    // Make the socket that will be used for the communication
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

    if (fd < 0)
    {
        goto fail;
    }

    // Allow the program to manage wait and timeouts
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    {
        goto fail;
    }

    // Start the connection and wait if it takes time to finish
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0)
    {
        int e = 0;
        socklen_t len = sizeof e;

        if (errno != EINPROGRESS || !ready(fd, POLLOUT, deadline))
        {
            goto fail;
        }

        // Check and see if the connection completed successfully
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len) < 0 || e)
        {
            if (e)
            {
                errno = e;
            }

            goto fail;
        }
    }

    printf("Connected to tracker %s:%s\nSending: %s",
           c->host, c->port, request);

    // A request might need a lot of sends before all bytes are sent
    while (sent < strlen(request))
    {
        if (!ready(fd, POLLOUT, deadline))
        {
            goto fail;
        }

        ssize_t n = send(fd, request + sent, strlen(request) - sent, 0);

        if (n < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        {
            continue;
        }

        if (n <= 0)
        {
            goto fail;
        }

        sent += (size_t)n;
    }

    // Set space for reply and null character in the end
    buf = malloc(LIMIT + 1);

    if (!buf)
    {
        goto fail;
    }

    for (;;)
    {
        char extra;

        if (!ready(fd, POLLIN, deadline))
        {
            goto fail;
        }

        // When the buffer is full, check and see if extra data is coming
        ssize_t n = recv(
            fd,
            used < LIMIT ? buf + used : &extra,
            used < LIMIT ? LIMIT - used : 1,
            0
        );

        if (n < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        {
            continue;
        }

        if (n < 0)
        {
            goto fail;
        }

        // 0 means that the tracker has closed the connection
        if (!n)
        {
            break;
        }

        if (used == LIMIT)
        {
            error("response exceeds 1 MiB");
            goto fail;
        }

        used += (size_t)n;
    }

    close(fd);
    freeaddrinfo(ai);

    // Check and see if the text reply contains an unexpected null character
    if (memchr(buf, 0, used))
    {
        error("NUL in text response");
        free(buf);
        return NULL;
    }

    // Mark the end of the text and return the number of bytes received
    buf[used] = 0;
    *size = used;

    return buf;

fail:
    // Close the connection and clear the memory after an error
    perror("tracker communication");

    if (fd >= 0)
    {
        close(fd);
    }

    freeaddrinfo(ai);
    free(buf);

    return NULL;
}

// Split the text into words using spaces, tabs and line endings
static int split(char *s, char **v, int max)
{
    int n = 0;
    char *save = NULL, *p = strtok_r(s, " \t\r\n", &save);

    while (p)
    {
        if (n == max)
        {
            return -1;
        }

        v[n++] = p;
        p = strtok_r(NULL, " \t\r\n", &save);
    }

    return n;
}

// Take the next complete line and move to the next part of the reply
static char *next_line(char **cursor)
{
    char *s = *cursor, *end = strchr(s, '\n');

    if (!end)
    {
        return NULL;
    }

    *end = 0;
    *cursor = end + 1;

    return s;
}

// Check and remove the angle brackets around the message
static int brackets(char *s)
{
    size_t n = strlen(s);

    if (n < 2 || s[0] != '<' || s[n - 1] != '>')
    {
        return 0;
    }

    memmove(s, s + 1, n - 2);
    s[n - 2] = 0;

    return 1;
}

// Read the LIST reply and show the available files
static int read_list(char *reply)
{
    char *cursor = reply, *line, *v[5];
    unsigned long long count, index, size;

    // Check the first line and get the number of files
    line = next_line(&cursor);

    if (!line ||
        !brackets(line) ||
        split(line, v, 5) != 3 ||
        strcmp(v[0], "REP") ||
        strcmp(v[1], "LIST") ||
        !number(v[2], &count) ||
        count > 10000)
    {
        return 0;
    }

    // Check the number, filename, size and checksum for each file
    for (unsigned long long i = 1; i <= count; ++i)
    {
        line = next_line(&cursor);

        if (!line ||
            !brackets(line) ||
            split(line, v, 5) != 4 ||
            !number(v[0], &index) ||
            index != i ||
            !filename(v[1]) ||
            !number(v[2], &size) ||
            !digest_text(v[3]))
        {
            return 0;
        }

        printf("%llu. %s | %llu bytes | MD5 %s\n",
               i, v[1], size, v[3]);
    }

    // Check and see if the reply has the correct ending
    if (strcmp(cursor, "<REP LIST END>\n"))
    {
        return 0;
    }

    printf("LIST complete: %llu file(s).\n", count);

    return 1;
}

// Check the file details and peer information inside the tracker file
static int check_info(const char *body, size_t len, const char *requested)
{
    char *copy = malloc(len + 1), *cursor, *line;
    int fields = 0, peers = 0, ok = 0;
    unsigned long long filesize = 0;

    if (!copy)
    {
        return 0;
    }

    // Make a copy of the received information to check
    memcpy(copy, body, len);
    copy[len] = 0;
    cursor = copy;

    while (*cursor)
    {
        // Separate the next line from the remaining text
        line = cursor;
        char *end = strchr(cursor, '\n');

        if (end)
        {
            *end = 0;
            cursor = end + 1;
        }
        else
        {
            cursor += strlen(cursor);
        }

        // Remove the extra character used in Windows line endings
        size_t l = strlen(line);

        if (l && line[l - 1] == '\r')
        {
            line[l - 1] = 0;
        }

        // Skip empty lines and comments
        if (!*line || *line == '#')
        {
            continue;
        }

        if (!strncmp(line, "Filename: ", 10))
        {
            char expected[200];

            // Check and see if the filename matches the requested tracker
            snprintf(expected, sizeof expected, "%s.track", line + 10);

            if (fields || !filename(line + 10) || strcmp(expected, requested))
            {
                goto done;
            }

            fields = 1;
        }
        else if (!strncmp(line, "Filesize: ", 10))
        {
            // Check the file size after reading the filename
            if (fields != 1 || !number(line + 10, &filesize))
            {
                goto done;
            }

            fields = 2;
        }
        else if (!strncmp(line, "Description:", 12))
        {
            // Allow a description after the file size
            if (fields != 2)
            {
                goto done;
            }

            fields = 3;
        }
        else if (!strncmp(line, "MD5: ", 5))
        {
            // Check the format and position of the file checksum
            if ((fields != 2 && fields != 3) || !digest_text(line + 5))
            {
                goto done;
            }

            fields = 4;
        }
        else
        {
            char *v[6], *p = line;
            unsigned long long port, start, finish, stamp;
            struct in_addr ip;

            // Make sure the file details come before the peer information
            if (fields != 4)
            {
                goto done;
            }

            // Separate the IP, port, start byte, end byte and timestamp
            for (int i = 0; i < 5; ++i)
            {
                v[i] = p;
                char *sep = strchr(p, ':');

                if (i < 4)
                {
                    if (!sep)
                    {
                        goto done;
                    }

                    *sep = 0;
                    p = sep + 1;
                }
                else if (sep)
                {
                    goto done;
                }
            }

            // Check the peer address and make sure the byte range fits the file
            if (inet_pton(AF_INET, v[0], &ip) != 1 ||
                !number(v[1], &port) ||
                port < 1 ||
                port > 65535 ||
                !number(v[2], &start) ||
                !number(v[3], &finish) ||
                start > finish ||
                finish > filesize ||
                !number(v[4], &stamp))
            {
                goto done;
            }

            ++peers;
        }
    }

    // Check and see if all the required file details were found
    ok = fields == 4;

    if (ok)
    {
        printf("Tracker parsed: %llu shared-file bytes, %d peer record(s).\n",
               filesize, peers);
    }

done:
    free(copy);

    return ok;
}

// Save the checked tracker file in the local cache
static int save_cache(
    const char *directory,
    const char *name,
    const char *body,
    size_t len
)
{
    char path[PATH_MAX], temp[PATH_MAX];
    int fd;
    FILE *f;

    // Make the cache folder if it does not already exist
    if (mkdir(directory, 0700) < 0 && errno != EEXIST)
    {
        perror(directory);
        return 0;
    }

    // Set the paths for the final file and temporary file
    if (snprintf(path, sizeof path, "%s/%s", directory, name) >=
            (int)sizeof path ||
        snprintf(temp, sizeof temp, "%s/.incoming-XXXXXX", directory) >=
            (int)sizeof temp)
    {
        return 0;
    }

    // Make a temporary file with a unique name
    fd = mkstemp(temp);

    if (fd < 0)
    {
        perror("cache temporary file");
        return 0;
    }

    f = fdopen(fd, "wb");

    if (!f)
    {
        close(fd);
        unlink(temp);
        return 0;
    }

    // Write the checked bytes into the temporary file
    int ok = fwrite(body, 1, len, f) == len;

    // Finish saving the data and close the file
    if (fflush(f) || fsync(fd))
    {
        ok = 0;
    }

    if (fclose(f))
    {
        ok = 0;
    }

    // Replace the cached file after the temporary file is ready
    if (ok && rename(temp, path) == 0)
    {
        printf("MD5 verified. Cached tracker: %s\n", path);
        return 1;
    }

    // Remove the temporary file if saving failed
    perror("cache write");
    unlink(temp);

    return 0;
}

// Check the GET reply, check its checksum and save the tracker data
static int read_get(char *reply, size_t len, const char *name, const char *cache)
{
    const char *begin = "<REP GET BEGIN>\n", *marker = "<REP GET END ";
    size_t head = strlen(begin), tail = strlen(marker) + 32 + 2;
    char expected[33], actual[33];

    // Check the starting message and make sure the reply is long enough
    if (len < head + tail || memcmp(reply, begin, head))
    {
        return 0;
    }

    // Find the ending message that contains the tracker checksum
    char *footer = reply + len - tail;

    if (memcmp(footer, marker, strlen(marker)) ||
        memcmp(reply + len - 2, ">\n", 2))
    {
        return 0;
    }

    // Copy and check the checksum sent by the tracker
    memcpy(expected, footer + strlen(marker), 32);
    expected[32] = 0;

    if (!digest_text(expected))
    {
        return 0;
    }

    // Find the tracker data between the starting and ending messages
    char *body = reply + head;
    size_t n = (size_t)(footer - body);

    // Calculate the checksum from the exact bytes received
    if (!md5(body, n, actual))
    {
        return 0;
    }

    if (strcasecmp(expected, actual))
    {
        // Check and see if the contents have the supported angle brackets around them
        if (n < 4 || body[0] != '<' || memcmp(body + n - 3, " >\n", 3))
        {
            error("MD5 mismatch");
            return 0;
        }

        // Skip the surrounding characters and check the checksum again
        ++body;
        n -= 4;

        if (!md5(body, n, actual) || strcasecmp(expected, actual))
        {
            error("MD5 mismatch");
            return 0;
        }
    }

    // Check the tracker information before saving it
    if (!check_info(body, n, name))
    {
        error("invalid tracker metadata");
        return 0;
    }

    if (!save_cache(cache, name, body, n))
    {
        return 0;
    }

    puts("Tracker retrieval complete.");

    return 1;
}

// Check the user's command, send it and get the reply
static int run(const Config *c, const char *cache, char *command)
{
    char *v[10], request[4096];
    int n = split(command, v, 10), kind = 0;
    unsigned long long a, b, port;
    struct in_addr ip;

    // Allow the user to type LIST or REQ LIST
    if (n == 1 && !strcasecmp(v[0], "LIST"))
    {
        kind = 1;
    }

    if (n == 2 && !strcasecmp(v[0], "REQ") && !strcasecmp(v[1], "LIST"))
    {
        kind = 1;
    }

    if (kind == 1)
    {
        strcpy(request, "<REQ LIST>\n");
    }
    else if (n == 2 &&
             !strcasecmp(v[0], "GET") &&
             filename(v[1]) &&
             strlen(v[1]) > 6 &&
             !strcmp(v[1] + strlen(v[1]) - 6, ".track"))
    {
        // Make the GET request using a valid tracker filename
        kind = 2;
        snprintf(request, sizeof request, "<GET %s >\n", v[1]);
    }
    else if (n == 7 &&
             !strcasecmp(v[0], "createtracker") &&
             filename(v[1]) &&
             number(v[2], &a) &&
             digest_text(v[4]) &&
             inet_pton(AF_INET, v[5], &ip) == 1 &&
             number(v[6], &port) &&
             port > 0 &&
             port <= 65535 &&
             !strpbrk(v[3], "<>"))
    {
        // Register the file using its details and the sharing peer's address
        kind = 3;

        snprintf(request, sizeof request,
                 "<createtracker %s %s %s %s %s %s>\n",
                 v[1], v[2], v[3], v[4], v[5], v[6]);
    }
    else if (n == 6 &&
             !strcasecmp(v[0], "updatetracker") &&
             filename(v[1]) &&
             number(v[2], &a) &&
             number(v[3], &b) &&
             a <= b &&
             inet_pton(AF_INET, v[4], &ip) == 1 &&
             number(v[5], &port) &&
             port > 0 &&
             port <= 65535)
    {
        // Update the range of bytes available from the sharing peer
        kind = 4;

        snprintf(request, sizeof request,
                 "<updatetracker %s %s %s %s %s>\n",
                 v[1], v[2], v[3], v[4], v[5]);
    }
    else
    {
        error("invalid command; use LIST, GET name.track, createtracker or updatetracker (see README)");
        return 0;
    }

    // Send the request and get the tracker reply
    size_t len;
    char *reply = talk(c, request, &len);
    int ok = 0;

    if (!reply)
    {
        return 0;
    }

    // Use the correct function to check the reply
    if (kind == 1)
    {
        ok = read_list(reply);
    }
    else if (kind == 2)
    {
        ok = read_get(reply, len, v[1], cache);
    }
    else
    {
        char expected[256];

        // Check the success reply for create and update commands
        if (kind == 3)
        {
            strcpy(expected, "<createtracker succ>\n");
        }
        else
        {
            snprintf(expected, sizeof expected,
                     "<updatetracker %s succ>\n", v[1]);
        }

        printf("Tracker response: %s", reply);
        ok = !strcmp(reply, expected);
    }

    if (!ok)
    {
        error("tracker rejected request or reply failed validation; cache unchanged");
    }

    free(reply);

    return ok;
}

// Load the settings and run the commands entered by the user
int main(int argc, char **argv)
{
    const char *cfg = "clientThreadConfig.cfg", *cache = "cache";
    int i = 1, ok = 1;
    Config c;

    // Allow the program to handle an error if the connection breaks while sending
    signal(SIGPIPE, SIG_IGN);

    // Read any configuration or cache paths entered by the user
    while (i < argc && argv[i][0] == '-')
    {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
        {
            cfg = argv[i + 1];
            i += 2;
        }
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)
        {
            cache = argv[i + 1];
            i += 2;
        }
        else
        {
            puts("Usage: peer [--config FILE] [--cache DIR] [COMMAND ...]");
            return 2;
        }
    }

    if (!load_cfg(cfg, &c))
    {
        return 1;
    }

    char command[2048];

    // Run the command if the user entered it when starting the program
    if (i < argc)
    {
        size_t used = 0;

        // Join the entered arguments into one command
        for (; i < argc; ++i)
        {
            size_t n = strlen(argv[i]);

            if (used + n + 2 > sizeof command)
            {
                error("command too long");
                return 2;
            }

            memcpy(command + used, argv[i], n);
            used += n;
            command[used++] = ' ';
        }

        command[used] = 0;

        return run(&c, cache, command) ? 0 : 1;
    }

    // Show the prompt if the user did not enter a command when starting
    puts("Peer client. Enter LIST, GET filename.track, createtracker, updatetracker, or quit.");

    for (;;)
    {
        printf("peer> ");
        fflush(stdout);

        if (!fgets(command, sizeof command, stdin))
        {
            break;
        }

        // Remove the remaining input if the command is too long
        if (!strchr(command, '\n') && !feof(stdin))
        {
            int ch;

            while ((ch = getchar()) != '\n' && ch != EOF)
            {
            }

            error("command too long");
            ok = 0;

            continue;
        }

        // Remove the line ending before checking the command
        command[strcspn(command, "\r\n")] = 0;

        if (!strcmp(command, "quit") || !strcmp(command, "exit"))
        {
            break;
        }

        if (!*command)
        {
            continue;
        }

        // Keep track of whether any command failed
        if (!run(&c, cache, command))
        {
            ok = 0;
        }
    }

    // Return 0 if all commands worked or 1 if a command failed
    return ok ? 0 : 1;
}