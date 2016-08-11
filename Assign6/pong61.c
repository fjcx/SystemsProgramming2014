#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>
#include "serverinfo.h"

#define MINDELAY 0.01    // min delay before reconnection attempt
#define MAXDELAY 128     // max delay for reconnection attempt
#define MAXTHREADS 30    // max number of threads
#define MAXCONNECT 30    // max number of connections
#define BIGBUFSIZ 25000  // bigger buffer size for phase 5

static const char* pong_host = PONG_HOST;
static const char* pong_port = PONG_PORT;
static const char* pong_user = PONG_USER;
static struct addrinfo* pong_addr;

int fadetime = -1;           // fade time (-1, doesn't pass fadetime)
int threadcnt = 0;           // count of concurrent open threads
struct timespec stopdelay;   // congestion delay time

pthread_mutex_t mutex;
pthread_mutex_t thrdcntmut;  // thread count mutex
pthread_mutex_t connpoolmut; // connection pool mutex
pthread_mutex_t stpdlymut;   // stop delay mutex
pthread_cond_t condvar;
pthread_cond_t stpdlycond;   // condition var for stop delay


// ** FUN-MODE vars **
// def of positions for a digit (excluding offset)
int digpos[15][2] = {{0,0},{0,1},{0,2},{0,3},{0,4}, 
                     {1,0},{1,1},{1,2},{1,3},{1,4},
                     {2,0},{2,1},{2,2},{2,3},{2,4}};

// def of points needed for digits 0-9
int digit[10][15] = {{1,1,1,1,1, 1,0,0,0,1, 1,1,1,1,1},  // 0
                     {0,0,0,0,0, 0,0,0,0,0, 1,1,1,1,1},  // 1
                     {1,0,1,1,1, 1,0,1,0,1, 1,1,1,0,1},  // 2
                     {1,0,1,0,1, 1,0,1,0,1, 1,1,1,1,1},  // 3
                     {1,1,1,0,0, 0,0,1,0,0, 1,1,1,1,1},  // 4
                     {1,1,1,0,1, 1,0,1,0,1, 1,0,1,1,1},  // 5
                     {1,1,1,1,1, 1,0,1,0,1, 1,0,1,1,1},  // 6
                     {1,0,0,0,0, 1,0,0,0,0, 1,1,1,1,1},  // 7
                     {1,1,1,1,1, 1,0,1,0,1, 1,1,1,1,1},  // 8
                     {1,1,1,0,1, 1,0,1,0,1, 1,1,1,1,1}}; // 9

// placeholder for current time
int currtime[6] = {0,0,0,0,0,0};

// update_clock_time
//   updating clock to current time
static void update_clock_time(void) {
    time_t now;
    struct tm *lcltime;
    now = time(NULL);
    lcltime = localtime(&now);
    currtime[1] = lcltime->tm_hour % 10;
    currtime[0] = (lcltime->tm_hour - currtime[1]) /10;
    currtime[3] = lcltime->tm_min % 10;
    currtime[2] = (lcltime->tm_min - currtime[3])/10;
    currtime[5] = lcltime->tm_sec % 10;
    currtime[4] = (lcltime->tm_sec - currtime[5])/10;
    printf ( "showing time: %d%d:%d%d:%d%d\n", currtime[0],currtime[1],
            currtime[2],currtime[3],currtime[4],currtime[5]);
}


// ** TIME HELPERS **
double elapsed_base = 0;

// timestamp()
//   Return the current absolute time as a real number of seconds.
double timestamp(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec + (double) now.tv_usec / 1000000;
}

// elapsed()
//   Return the number of seconds that have elapsed since `elapsed_base`.
double elapsed(void) {
    return timestamp() - elapsed_base;
}

// set_stop_delay
//   setting the delay time when 'stop' signal is recieved
//   to allievate congestions.
void set_stop_delay(int delay_msec) {
    struct timespec newdelay;
    struct timeval now;
    gettimeofday(&now, NULL);
    int delay_usec = delay_msec * 1000;
    int tot_usec = now.tv_usec + delay_usec;
    
    // convert to sec and nano-sec
    newdelay.tv_sec = now.tv_sec + (tot_usec/1000000);
    newdelay.tv_nsec = (tot_usec%1000000)*1000;
    fprintf(stderr, "delay until %ld.%06ldsec\n",newdelay.tv_sec,newdelay.tv_nsec);
        
    // only set delay if longer than current delay
    pthread_mutex_lock(&stpdlymut);
    stopdelay.tv_sec = newdelay.tv_sec;
    stopdelay.tv_nsec = newdelay.tv_nsec;
    pthread_mutex_unlock(&stpdlymut);
}

// reset_stop_delay
//   helper method to reset the stop delay back to 0
void reset_stop_delay(void) {
    stopdelay.tv_sec = 0;
    stopdelay.tv_nsec = 0;
}


// ** HTTP CONNECTION MANAGEMENT **

// http_connection
//    This object represents an open HTTP connection to a server.
typedef struct http_connection http_connection;
struct http_connection {
    int fd;                 // Socket file descriptor

    int state;              // Response parsing status (see below)
    int status_code;        // Response status code (e.g., 200, 402)
    size_t content_length;  // Content-Length value
    int has_content_length; // 1 iff Content-Length was provided
    int eof;                // 1 iff connection EOF has been reached

    char buf[BIGBUFSIZ];    // Response buffer
    size_t len;             // Length of response buffer
};

http_connection *connectionpool[MAXCONNECT]; // connection table

// `http_connection::state` constants
#define HTTP_REQUEST 0      // Request not sent yet
#define HTTP_INITIAL 1      // Before first line of response
#define HTTP_HEADERS 2      // After first line of response, in headers
#define HTTP_BODY    3      // In body
#define HTTP_DONE    (-1)   // Body complete, available for a new request
#define HTTP_CLOSED  (-2)   // Body complete, connection closed
#define HTTP_BROKEN  (-3)   // Parse error

// helper functions
char* http_truncate_response(http_connection* conn);
static int http_process_response_headers(http_connection* conn);
static int http_check_response_body(http_connection* conn);

static void usage(void);


// http_connect(ai)
//    Open a new connection to the server described by `ai`. Returns a new
//    `http_connection` object for that server connection. Exits with an
//    error message if the connection fails.
http_connection* http_connect(const struct addrinfo* ai) {
    // connect to the server
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    int yes = 1;
    (void) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (r < 0) {
        perror("connect");
        exit(1);
    }

    // construct an http_connection object for this connection
    http_connection* conn =
        (http_connection*) malloc(sizeof(http_connection));
    conn->fd = fd;
    conn->state = HTTP_REQUEST;
    conn->eof = 0;
    return conn;
}


// http_close(conn)
//    Close the HTTP connection `conn` and free its resources.
void http_close(http_connection* conn) {
    close(conn->fd);
    free(conn);
}

// get_connection
//   Looks in the connectino table for existing threads it can use.
//   If no reusable threads are found then a new one is initialized by
//   calling http_connect(). If no connections are free then NULL is 
//   returned to the caller.
http_connection* get_connection(const struct addrinfo* ai) {
    int free_slot = -1;
    pthread_mutex_lock(&connpoolmut);
    // look for existing connection, that can be used
    for (int i=0; i < MAXCONNECT; i++) {
        // connection can be reused if it's not null and has finished,
        // indicated by the HTTP_DONE state.
        if (connectionpool[i] != NULL && connectionpool[i]->state == HTTP_DONE) {
            pthread_mutex_unlock(&connpoolmut);
            return connectionpool[i];
        } else if (connectionpool[i] == NULL && free_slot == -1) {
            // An uninitialized connection is identified as a NULL slot.
            // note place of first free slot
            free_slot = i;
        }
    }
    
    // if no existing is found then init another, in an unused slot !!!
    if (free_slot != -1) {
        connectionpool[free_slot] = http_connect(ai);
        pthread_mutex_unlock(&connpoolmut);
        return connectionpool[free_slot];
    }
    pthread_mutex_unlock(&connpoolmut);
    // if no connection currently available, then return NULL
    return NULL;
}


// http_send_request(conn, uri)
//    Send an HTTP POST request for `uri` to connection `conn`.
//    Exit on error.
void http_send_request(http_connection* conn, const char* uri) {
    assert(conn->state == HTTP_REQUEST || conn->state == HTTP_DONE);

    // prepare and write the request
    char reqbuf[BUFSIZ];
    size_t reqsz = sprintf(reqbuf,
                           "POST /%s/%s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "Connection: keep-alive\r\n"
                           "\r\n",
                           pong_user, uri, pong_host);
    size_t pos = 0;
    while (pos < reqsz) {
        ssize_t nw = write(conn->fd, &reqbuf[pos], reqsz - pos);
        if (nw == 0)
            break;
        else if (nw == -1 && errno != EINTR && errno != EAGAIN) {
            perror("write");
            exit(1);
        } else if (nw != -1)
            pos += nw;
    }

    if (pos != reqsz) {
        fprintf(stderr, "%.3f sec: connection closed prematurely\n",
                elapsed());
        exit(1);
    }

    // clear response information
    conn->state = HTTP_INITIAL;
    conn->status_code = -1;
    conn->content_length = 0;
    conn->has_content_length = 0;
    conn->len = 0;
}


// http_receive_response_headers(conn)
//    Read the server's response headers. On return, `conn->status_code`
//    holds the server's status code. If the connection terminates
//    prematurely, `conn->status_code` is -1.
void http_receive_response_headers(http_connection* conn) {
    assert(conn->state != HTTP_REQUEST);
    if (conn->state < 0)
        return;

    // read & parse data until told `http_process_response_headers`
    // tells us to stop
    while (http_process_response_headers(conn)) {
        ssize_t nr = read(conn->fd, &conn->buf[conn->len], BIGBUFSIZ);
        if (nr == 0)
            conn->eof = 1;
        else if (nr == -1 && errno != EINTR && errno != EAGAIN) {
            perror("read");
            exit(1);
        } else if (nr != -1)
            conn->len += nr;
    }

    // Status codes >= 500 mean we are overloading the server
    // and should exit.
    if (conn->status_code >= 500) {
        fprintf(stderr, "%.3f sec: exiting because of "
                "server status %d (%s)\n", elapsed(),
                conn->status_code, http_truncate_response(conn));
        exit(1);
    }
}


// http_receive_response_body(conn)
//    Read the server's response body. On return, `conn->buf` holds the
//    response body, which is `conn->len` bytes long and has been
//    null-terminated.
void http_receive_response_body(http_connection* conn) {
    assert(conn->state < 0 || conn->state == HTTP_BODY);
    if (conn->state < 0)
        return;

    // read response body (http_check_response_body tells us when to stop)
    while (http_check_response_body(conn)) {
        ssize_t nr = read(conn->fd, &conn->buf[conn->len], BIGBUFSIZ);
        if (nr == 0)
            conn->eof = 1;
        else if (nr == -1 && errno != EINTR && errno != EAGAIN) {
            perror("read");
            exit(1);
        } else if (nr != -1)
            conn->len += nr;
    }

    // null-terminate body
    conn->buf[conn->len] = 0;
}


// http_truncate_response(conn)
//    Truncate the `conn` response text to a manageable length and return
//    that truncated text. Useful for error messages.
char* http_truncate_response(http_connection* conn) {
    char *eol = strchr(conn->buf, '\n');
    if (eol)
        *eol = 0;
    if (strnlen(conn->buf, 100) >= 100)
        conn->buf[100] = 0;
    return conn->buf;
}


// ** MAIN PROGRAM **

typedef struct pong_args {
    int x;
    int y;
} pong_args;


// pong_thread(threadarg)
//    Connect to the server at the position indicated by `threadarg`
//    (which is a pointer to a `pong_args` structure).
void* pong_thread(void* threadarg) {
    pthread_detach(pthread_self());

    // Copy thread arguments onto our stack.
    pong_args pa = *((pong_args*) threadarg);

    char url[256];
    if (fadetime == -1) {
        snprintf(url, sizeof(url), "move?x=%d&y=%d&style=on",
             pa.x, pa.y);
    } else {
        snprintf(url, sizeof(url), "move?x=%d&y=%d&style=on&fade=%d",
             pa.x, pa.y, fadetime);
    }
             
    int connstat = -1;    	      // connection status loop check !!!
    double delay = MINDELAY/2;    // just start min_delay as half, cause it is doubled
    http_connection* conn;
    while (connstat != 200) {
        while ((conn = get_connection(pong_addr)) == NULL) {
            // if no connection available, sleep for a bit and try again
            usleep(100000);
        };
        http_send_request(conn, url);
        http_receive_response_headers(conn);
        connstat = conn->status_code;
        
        if (conn->status_code == -1) {
            // close conn and retry with exponential back-off !!!
            pthread_mutex_lock(&connpoolmut);
            // if connection errors out, then shut it down !!!
            for (int i = 0; i < MAXCONNECT; i++) {
                if (connectionpool[i] == conn) {
                    connectionpool[i] = NULL;
                    http_close(conn);
                }
            }
            pthread_mutex_unlock(&connpoolmut);
        
            if (delay >= MAXDELAY) {
                delay = MAXDELAY;
            } else {
                delay = 2*delay;
            }
            fprintf(stderr, "%.3f sec: warning: %d,%d: "
                    "server returned status %d (expected 200), retrying in %.2f sec\n",
                    elapsed(), pa.x, pa.y, connstat, delay);
            usleep((long) (delay * 1000000));
        }
    }
    // allowing main thread to continue after recieving header
    pthread_cond_signal(&condvar);
    http_receive_response_body(conn);
    double result = strtod(conn->buf, NULL);
    if (result < 0) {
        fprintf(stderr, "%.3f sec: server returned error: %s\n",
                elapsed(), http_truncate_response(conn));
        exit(1);
    } else if (result > 0) {
        // congestion occuring, so set stop delay
        set_stop_delay(result);
    }

    // thread exiting, so decrement thread count
    pthread_mutex_lock(&thrdcntmut);
    --threadcnt;
    pthread_mutex_unlock(&thrdcntmut);
    pthread_exit(NULL);
}


// usage()
//    Explain how pong61 should be run.
static void usage(void) {
    fprintf(stderr, "Usage: ./pong61 [-h HOST] [-p PORT] [USER]\n");
    exit(1);
}


// main(argc, argv)
//    The main loop.
int main(int argc, char** argv) {
    // parse arguments
    int ch, nocheck = 0;
    while ((ch = getopt(argc, argv, "nh:p:u:")) != -1) {
        if (ch == 'h')
            pong_host = optarg;
        else if (ch == 'p')
            pong_port = optarg;
        else if (ch == 'u')
            pong_user = optarg;
        else if (ch == 'n')
            nocheck = 1;
        else
            usage();
    }
    if (optind == argc - 1)
        pong_user = argv[optind];
    else if (optind != argc)
        usage();

    // look up network address of pong server
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    int r = getaddrinfo(pong_host, pong_port, &hints, &pong_addr);
    if (r != 0) {
        fprintf(stderr, "problem looking up %s: %s\n",
                pong_host, gai_strerror(r));
        exit(1);
    }

    // rd: pong board and get its dimensions
    int width, height;
    {
        http_connection* conn = http_connect(pong_addr);
        http_send_request(conn, nocheck ? "reset?nocheck=1" : "reset");
        http_receive_response_headers(conn);
        http_receive_response_body(conn);
        if (conn->status_code != 200
            || sscanf(conn->buf, "%d %d\n", &width, &height) != 2
            || width <= 0 || height <= 0) {
            fprintf(stderr, "bad response to \"reset\" RPC: %d %s\n",
                    conn->status_code, http_truncate_response(conn));
            exit(1);
        }
        http_close(conn);
    }
    // measure future times relative to this moment
    elapsed_base = timestamp();

    // print display URL
    printf("Display: http://%s:%s/%s/%s\n",
           pong_host, pong_port, pong_user,
           nocheck ? " (NOCHECK mode)" : "");

    // initialize global synchronization objects
    pthread_mutex_init(&mutex, NULL);
    pthread_mutex_init(&thrdcntmut, NULL);
    pthread_mutex_init(&connpoolmut, NULL);
    pthread_mutex_init(&stpdlymut, NULL);
    pthread_cond_init(&condvar, NULL);
    pthread_cond_init(&stpdlycond, NULL);
    
    // init connection table to NULL (IS THIS NECESSARY ???, prob not)
    pthread_mutex_lock(&connpoolmut);
    for (int i=0; i < MAXCONNECT; i++) {
        connectionpool[i] = NULL;
    }
    pthread_mutex_unlock(&connpoolmut);   

    // play game
    int x = 0, y = 0, dx = 1, dy = 1;
    char url[BUFSIZ];
    int threadcntvar;
    
    // fun-mode vars, and init
    int f_indx = 0;
    int digcnt = 0;
    int xoffset = width/3;
    int yoffset = 1;
    int clock_updated = 0;
    int do_send_dot = 1;
    if (nocheck == 1) {
        fadetime = 25000;
        update_clock_time();
    }    
    
    while (1) {
        pthread_mutex_lock(&thrdcntmut);
        threadcntvar = threadcnt;
        pthread_mutex_unlock(&thrdcntmut);
        // don't create too many threads !!
        if (threadcntvar < MAXTHREADS) {
            if (nocheck == 1) {
                // fun-mode - (clock functionality)
                // displays current time,
                // waits a few seconds
                // then updates and display new time
                if (clock_updated == 1) {
                    // set next time and 
                    // position in grid
                    usleep(5000000);
                    digcnt = 0;
                    xoffset = width/3;
                    yoffset = 1;
                    clock_updated = 0;
                    update_clock_time();
                }
                
                if (digit[currtime[digcnt]][f_indx] == 1) {
                    x = digpos[f_indx][0] + xoffset;
                    y = digpos[f_indx][1] + yoffset;
                    do_send_dot = 1;
                } else {
                    do_send_dot = 0;
                }
                
                if (f_indx < 14) {
                    ++f_indx;
                } else {
                    f_indx = 0;
                    if (digcnt < 5) {
                        ++digcnt;
                        if (digcnt == 2 || digcnt == 4) {
                            // end of hrs, and mins
                            xoffset = width/3;
                            yoffset +=6;
                        } else if (xoffset < (width - 8)) {
                            xoffset += 4;
                        } else {
                            xoffset = 0; // reset if we hit edge
                        }
                    } else {
                        clock_updated = 1;
                    }
                }
            }
        
            if (do_send_dot == 1) {
                // create a new thread to handle the next position
                pong_args pa;
                pa.x = x;
                pa.y = y;
                pthread_t pt;
                
                pthread_mutex_lock(&thrdcntmut);
                // about to create new thread so increment cnt
                ++threadcnt;    
                pthread_mutex_unlock(&thrdcntmut);

                pthread_mutex_lock(&stpdlymut);
                int rc = pthread_cond_timedwait(&stpdlycond, &stpdlymut, &stopdelay);
                if(rc == EINVAL || rc == EPERM || rc != ETIMEDOUT){
                    perror("Error - Request did not wait.");
                }
                reset_stop_delay();
                pthread_mutex_unlock(&stpdlymut);
                
                r = pthread_create(&pt, NULL, pong_thread, &pa);
                if (r != 0) {
                    fprintf(stderr, "%.3f sec: pthread_create: %s\n",
                            elapsed(), strerror(r));
                    exit(1);
                }

                // wait until that thread signals us to continue
                pthread_mutex_lock(&mutex);
                pthread_cond_wait(&condvar, &mutex);
                pthread_mutex_unlock(&mutex);
            }
            // update position
            if (nocheck == 0) {
                x += dx;
                y += dy;
                if (x < 0 || x >= width) {
                    dx = -dx;
                    x += 2 * dx;
                }
                if (y < 0 || y >= height) {
                    dy = -dy;
                    y += 2 * dy;
                }
            }
        }
        // wait 0.1sec
        usleep(100000);
    }
    
    // cleanup - close any open connections
    pthread_mutex_lock(&connpoolmut);
    for (int i=0; i < MAXCONNECT; i++) {
        http_close(connectionpool[i]);
    }
    pthread_mutex_unlock(&connpoolmut);  
}


// ** HTTP PARSING **

// http_process_response_headers(conn)
//    Parse the response represented by `conn->buf`. Returns 1
//    if more header data remains to be read, 0 if all headers
//    have been consumed.
static int http_process_response_headers(http_connection* conn) {
    size_t i = 0;
    while ((conn->state == HTTP_INITIAL || conn->state == HTTP_HEADERS)
           && i + 2 <= conn->len) {
        if (conn->buf[i] == '\r' && conn->buf[i+1] == '\n') {
            conn->buf[i] = 0;
            if (conn->state == HTTP_INITIAL) {
                int minor;
                if (sscanf(conn->buf, "HTTP/1.%d %d", &minor, &conn->status_code) == 2) {
                    conn->state = HTTP_HEADERS;
                } else {
                    conn->state = HTTP_BROKEN;
                }
            } else if (i == 0) {
                conn->state = HTTP_BODY;
            } else if (strncmp(conn->buf, "Content-Length: ", 16) == 0) {
                conn->content_length = strtoul(conn->buf + 16, NULL, 0);
                conn->has_content_length = 1;
            }
            memmove(conn->buf, conn->buf + i + 2, conn->len - (i + 2));
            conn->len -= i + 2;
            i = 0;
        } else {
            ++i;
        }
    }

    if (conn->eof) {
        conn->state = HTTP_BROKEN;
    }
    return conn->state == HTTP_INITIAL || conn->state == HTTP_HEADERS;
}


// http_check_response_body(conn)
//    Returns 1 if more response data should be read into `conn->buf`,
//    0 if the connection is broken or the response is complete.
static int http_check_response_body(http_connection* conn) {
    if (conn->state == HTTP_BODY
        && (conn->has_content_length || conn->eof)
        && conn->len >= conn->content_length)
        conn->state = HTTP_DONE;
    if (conn->eof && conn->state == HTTP_DONE)
        conn->state = HTTP_CLOSED;
    else if (conn->eof)
        conn->state = HTTP_BROKEN;
    return conn->state == HTTP_BODY;
}