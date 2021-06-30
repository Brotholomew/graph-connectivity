#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h> 
#include <sys/types.h>

#define FIFO_FILE "graph.fifo"

// error macro
#define ERR(msg) (perror(msg),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE));

void usage(int argc, char ** argv);

void setHandler(int sig, void (*handler)(int));
void sigchld_handler(int sig);
void catch(int sig);

void su_work(int ** fildes, int n, int fd);
void node_work(int ** fildes, int n, int num);
void write_to_node(int ** fildes, int n, char c[]);

void read_from_fifo(int fildes, int ** ftable, int n);
void procreate(int ** fildes, int n);
void close_unused_fildes(int ** fildes, int n, int exclude);
void close_fildes(int ** fildes, int n, int num);
void free_2d_array(int ** fildes, int n);

void split(char c[], int start_pos, int n, int * space_pos, unsigned char * size, unsigned char * x, unsigned char * y);
void sanitize(unsigned char x, unsigned char y, int * flag, int n);
void search(int ** fildes, int n, int num, int connected[], unsigned char * c, int size);
void pack_back(int ** fildes, int n, int num, int connected[], unsigned char * c, int size);

volatile sig_atomic_t last_sig = 0;

int main(int argc, char ** argv) {
    usage(argc, argv);
    int fifo, fd = 0, n;
    int ** fildes = NULL;

    n = atoi(argv[1]);
    if ((fildes = (int **) malloc((n + 1) * sizeof(int *))) == NULL) ERR("malloc");

    setHandler(SIGPIPE, SIG_IGN);
    setHandler(SIGCHLD, sigchld_handler);
    setHandler(SIGINT, catch);

    umask(0);
    errno = 0; 
    if ((fifo = mkfifo(FIFO_FILE, 0666)) < 0) if(errno!=EEXIST) ERR("create fifo");

    if (errno == EEXIST) {
        unlink(FIFO_FILE);
        if ((fifo = mkfifo(FIFO_FILE, 0666)) < 0) if(errno!=EEXIST) ERR("create fifo");
    }

    procreate(fildes, n);
    su_work(fildes, n, fd);
    
    close_fildes(fildes, n + 1, 1);
    close_fildes(fildes, n + 1, 0);
    free_2d_array(fildes, n + 1);
    while(wait(NULL)>0);
    return EXIT_SUCCESS;
}

void su_work(int ** fildes, int n, int fd) {
    if ((fd = open(FIFO_FILE, O_RDONLY)) < 0) if (errno != EINTR) ERR("could not open FIFO_FILE");

    if (last_sig != SIGINT)
        read_from_fifo(fd, fildes, n);
 
    kill(0, SIGINT);
}

void read_from_fifo(int fildes, int ** ftable, int n) {
    int m;
    char c[PIPE_BUF];
    memset(c, '\0', PIPE_BUF);

    do {
        errno = 0; m= 0;
        if ((m = read(fildes, c, PIPE_BUF)) < 0) if (errno != EINTR) ERR("read");
        if (errno == EINTR) break;
        if (m > 0) write_to_node(ftable, n, c);
    } while(m > 0);
    
}

void procreate(int ** fildes, int n) {
    for (int i = 0; i <= n; i++) {
        if ((fildes[i] = (int *) malloc(2 * sizeof(int))) == NULL) ERR("malloc");
        if (pipe(fildes[i])) ERR("pipe");
    }

    for (int i = 0; i < n; i++) {
        switch(fork()) {
            case 0:
                close_unused_fildes(fildes, n + 1, i);

                node_work(fildes, n, i);

                close_fildes(fildes, n + 1, 1);
                if (close(fildes[i][0])) ERR("close");
                //free(fildes);
                free_2d_array(fildes, n + 1);

                exit(EXIT_SUCCESS);
                break;
            case -1:
                ERR("fork");
                break;
        }
    }
}

void write_to_node(int ** fildes, int n, char c[]) {
    unsigned char x, y, size = 1, * buf = malloc(1); if (buf == NULL) ERR("malloc");
    int space_pos = 0, f = 1;

    /*
    frames schema:
    -------------------------------------------------------------------------
    | length [1] | mode ('p' / 'a' / 's' / 'r') [1] | contents [length - 1] |
    -------------------------------------------------------------------------
    */

    if (memcmp(c, "print", 5) == 0) {
        if ((buf = realloc(buf, (size + 1) * sizeof(unsigned char))) == NULL) ERR("realloc");
        
        buf[0] = size;
        buf[1] = 'p';

        for (int i = 0; i < n; i++) {
            if (TEMP_FAILURE_RETRY(write(fildes[i][1], buf, size + 1)) < size + 1) ERR("write");
        }
    } 
    
    if (memcmp(c, "add", 3) == 0) {
        split(c, 4, PIPE_BUF, &space_pos, &size, &x, &y);
        sanitize(x, y, &f, n);

        if (f) {
            if ((buf = realloc(buf, (3) * sizeof(unsigned char))) == NULL) ERR("realloc");
            buf[0] = 2;
            buf[1] = 'a';
            buf[2] = y;

            if (TEMP_FAILURE_RETRY(write(fildes[x][1], buf, 3)) < 3) ERR("write");  
        }  
    }
    

    if (memcmp(c, "conn", 4) == 0) {
        split(c, 5, PIPE_BUF, &space_pos, &size, &x, &y);
        sanitize(x, y, &f, n);

        if (f) {
            unsigned char response = 7;
            if ((buf = realloc(buf, (3) * sizeof(unsigned char))) == NULL) ERR("realloc");
            buf[0] = 2;
            buf[1] = 's';
            buf[2] = y;

            printf("%d \n", y);

            if (TEMP_FAILURE_RETRY(write(fildes[x][1], buf, 3)) < 3) ERR("write");  
            if (TEMP_FAILURE_RETRY(read(fildes[n][0], &response, 1)) < 1) ERR("read");

            if (response)
                printf("nodes %d and %d are connected \n", x, y);
            else
                printf("nodes %d and %d are not connected \n", x, y);
        }
    }

    if (buf != NULL) free(buf);
}

void sanitize(unsigned char x, unsigned char y, int * flag, int n) {
    if (x >= n || x < 0) {
        printf("no such vertex as %d \n", x);
        *flag = 0;
    }

    if (y >= n || y < 0) {
        printf("no such vertex as %d \n", y);
        *flag = 0;
    }
}

void split(char c[], int start_pos, int n, int * space_pos, unsigned char * size, unsigned char * x, unsigned char * y) {
    for (int i = start_pos; i < n; i++) {
        if (c[i] == '\0') break;
        if (c[i] == ' ') {
            *space_pos = i;
        }
        (*size)++;
    }

    *x = atoi(c + start_pos), *y = atoi(c + *space_pos + 1);
}

void node_work(int ** fildes, int n, int num) {
    int connected[n], ret;
    unsigned char buf[PIPE_BUF], size;
    unsigned char * c = malloc(0);
    
    for (int i = 0; i < n; i++) connected[i] = 0;

    while (last_sig != SIGINT) {
        memset(buf, '\0', PIPE_BUF);

        ret = (read(fildes[num][0], &size, 1));
        if (ret == 0) break;
        if (errno == EINTR) break;
        if (ret < 0 && errno != 32) ERR("read");

        c = realloc(c, size * sizeof(unsigned char));
        ret = TEMP_FAILURE_RETRY(read(fildes[num][0], c, size));

        if (size == 1 && c[0] == 'p') {
            printf("[NODE NO #%d] connected to: ", num);
            int f = 0;

            for (int i = 0; i < n; i++)
                if (connected[i]) {
                    printf("%d ", i);
                    f = 1;
                }

            if (f == 0)
                printf("no nodes");
            
            printf("\n");
        }

        if (size > 1 && c[0] == 'a') {
            int node = (int) c[1];
            if (node < n && node >= 0)
                connected[node] = 1;
        }

        if (size >= 1 && c[0] == 's') {
            printf("[PID #%d] received 's' packet: %d ", num, size); 
            for (int i = 0; i < size; i++)
                printf("%d ", c[i]);
            printf("\n");

            search(fildes, n, num, connected, c, size);
        } else if (size >= 1 && c[0] == 'r') {
            printf("[PID #%d] received 'r' packet: %d ", num, size); 
            for (int i = 0; i < size; i++)
                printf("%d ", c[i]);
            printf("\n");

            pack_back(fildes, n, num, connected, c, size);    
        }
        /*
        ------------------------------------------------
        | length_of_visited + 1 | 's' / 'r' | node / result | visited |
        ------------------------------------------------*/
        size = 0;
    }
    if (c != NULL) free(c);
}

/*
frame details for search 's' and response 'r' modes:
 ------------------------------------
 | length [1] | mode [1] | contents |
 ------------------------------------
 (array c[] does not include length, c[0] := mode)

 mode == 's'
 --------------------------------------------------------------
 | length [1] | 's' [1] | node_number [1] | array[length - 2] |
 --------------------------------------------------------------
 node_number -> the node the algorithm is searching for
 array[] -> previously visited nodes (current search path)

 mode == 'r'
 ------------------------------------------------------------------------
 | length [1] | 'r' [1] | response_code (0 / 1) [1] | array[length - 2] |
 ------------------------------------------------------------------------
 response_code: 0 - algorithm did not find the searched node; 1 - success!
 array[] -> previously visited nodes (used as a path to return response_code)
*/

void search(int ** fildes, int n, int num, int connected[], unsigned char * c, int size) {
    int node = c[1];
    if (size == 2 && connected[node]) {
        char result = 1;
        printf("[PID #%d]{search} found node %d, will send packet: 1 \n", num, node);
        if (TEMP_FAILURE_RETRY(write(fildes[n][1], &result, 1)) < 1) ERR("write");
        return;
    }

    if (connected[node]) {
        c[0] = 'r';
        c[1] = 1;

        printf("[PID #%d]{search} found node %d, will pack_back packet: ", num, node);
        printf("%d ", size);
        for (int i = 0; i < size; i++)
            printf("%d ", c[i]);
        printf("\n");

        pack_back(fildes, n, num, connected, c, size);
        return;
    }

    int visited[n];
    for (int i = 0; i < n; i++) {
        visited[i] = 0;
    }

    for (int i = 2; i < size; i++) {
        visited[c[i]] = 1;
    }

    int flagged = 0;
    for (int i = 0; i < n; i++) {
        if (connected[i] && visited[i] == 0) {
            unsigned char * forward = malloc((size + 2) * sizeof(unsigned char)); if (forward == NULL) ERR("malloc");
            unsigned char * temp = malloc((size + 1) * sizeof(unsigned char)); if (temp == NULL) ERR("malloc");

            if (memcpy(forward + 1, c, size) != (forward + 1)) ERR("memcpy");
            forward[0] = size + 1; // new size
            forward[size + 1] = num; // mark this vertix as visited

            printf("[PID #%d]{search} considering node %d, will send packet: ", num, i);
            for (int i = 0; i < size + 2; i++)
                printf("%d ", forward[i]);
            printf("\n");

            if (TEMP_FAILURE_RETRY(write(fildes[i][1], forward, size + 2)) < size + 2) ERR("write");
            if (TEMP_FAILURE_RETRY(read(fildes[num][0], temp, size + 1)) < size + 1) ERR("read");

            if (temp[2])
                flagged++;
            free(forward);
        }
    }

    if (flagged > 0) {
        c[0] = 'r';
        c[1] = 1;

        printf("[PID #%d]{search} determined that connection exists, will pack_back packet: ", num);
        printf("%d ", size);
        for (int i = 0; i < size; i++)
            printf("%d ", c[i]);
        printf("\n");

        pack_back(fildes, n, num, connected, c, size);
    }

    if (flagged == 0) {
        c[0] = 'r';
        c[1] = 0;

        printf("[PID #%d]{search} determined that connection does not exist, will pack_back packet: ", num);
        printf("%d ", size);
        for (int i = 0; i < size; i++)
            printf("%d ", c[i]);
        printf("\n");

        pack_back(fildes, n, num, connected, c, size);
    }
}

void pack_back(int ** fildes, int n, int num, int connected[], unsigned char * c, int size) {
    int result = c[1];
    if (size == 2) {

        printf("[PID #%d]{pack_back} the end of route! Will pack_back packet: %d \n", num, result);

        if (TEMP_FAILURE_RETRY(write(fildes[n][1], &result, 1)) < 1) ERR("write");
        return;
    }

    unsigned char * forward = malloc((size) * sizeof(unsigned char)); if (forward == NULL) ERR("malloc");
    
    if (memcpy(forward + 1, c, size - 1) != (forward + 1)) ERR("memcpy");
    forward[0] = size - 1; // new size

    printf("[PID #%d]{pack_back} considering node %d, will send packet: ", num, c[size - 1]);
    for (int i = 0; i < size; i++)
        printf("%d ", forward[i]);
    printf("\n");

    if (TEMP_FAILURE_RETRY(write(fildes[c[size - 1]][1], forward, size)) < size) ERR("write");
    free(forward);
}

void close_unused_fildes(int ** fildes, int n, int exclude) {
    for (int i = 0; i < n; i++) {
        if (i != exclude) {
            if (close(fildes[i][0])) ERR("close");
        }
    }
}

void close_fildes(int ** fildes, int n, int num) {
    for (int i = 0; i < n; i++) {
        if (close(fildes[i][num])) ERR("close");
    }
}

void free_2d_array(int ** fildes, int n) {
    for (int i = 0; i < n; i++) {
        free(fildes[i]);
    }

    free(fildes);
}

void usage(int argc, char ** argv) {
    if (argc < 2) {
        printf("[USAGE] %s <vertices_count>", argv[0]);
        exit(EXIT_FAILURE);
    }
}

void setHandler(int sig, void (*handler)(int)) {
    struct sigaction s1;
    memset(&s1,0,sizeof(struct sigaction));
    s1.sa_handler = handler;

    if(sigaction(sig,&s1,NULL)) ERR("sigaction");
}

void sigchld_handler(int sig) {
        pid_t pid;
        for(;;){
                pid=waitpid(0, NULL, WNOHANG);
                if(0==pid) return;
                if(0>=pid) {
                        if(ECHILD==errno) return;
                        ERR("waitpid:");
                }
        }
}

void catch(int sig) {
    last_sig = SIGINT;
}