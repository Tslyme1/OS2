#include "sync_pipe.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int start_fd;
int write_fd;

int sync_pipe_init() {
    int pipe_fds[2];
    int pipe_res = pipe(pipe_fds);
    if (pipe_res != 0) {
        perror("Error pipe():");
    }
    start_fd = pipe_fds[0];
    write_fd = pipe_fds[1];
    int fcntl_res = fcntl(start_fd, F_SETFL, O_NONBLOCK);
    if (fcntl_res < 0) {
        perror("make new client nonblock");
        close(start_fd);
        close(write_fd);
        return -1;
    }
    return pipe_res;
}

void sync_pipe_close() {
    close(start_fd);
    close(write_fd);
}

int sync_pipe_wait() {
    char buf;
    ssize_t was_read = read(start_fd, &buf, 1);
    if (was_read < 0) {
        perror("Error read for synchronization");
    }
    return (int)was_read;
}

void sync_pipe_notify(int num_really_created_threads) {
    char start_buf[BUFSIZ];
    ssize_t bytes_written = 0;
    while (bytes_written < num_really_created_threads) {
        ssize_t written = 0;
        if (num_really_created_threads - bytes_written <= BUFSIZ) {
            written = write(write_fd, start_buf, num_really_created_threads - bytes_written);
        }
        else {
            written = write(write_fd, start_buf, BUFSIZ);
        }
        if (written < 0) {
            perror("Error write");
            fprintf(stderr, "bytes_written: %ld / %d\n", bytes_written, num_really_created_threads);
        }
        else {
            bytes_written += written;
        }
    }
}

int get_rfd_spipe() {
    return start_fd;
}