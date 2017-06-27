#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <event2/event.h>

#include "debug.h"
#include "event_handlers.h"
#include "parser.h"
#include "pipe.h"
#include "stats.h"

#ifndef MAX_BYTES_TO_SPLICE
#define MAX_BYTES_TO_SPLICE 65536
#endif /* MAX_BYTES_TO_SPLICE */

int fd_dev_null = -1;

int open_dev_null(void) {
    fd_dev_null = open("/dev/null", O_WRONLY|O_NONBLOCK);
    if (fd_dev_null < 0)
        REPORT_ERROR(strerror(errno));
    return fd_dev_null;
}

void close_dev_null(void) {
    if (fd_dev_null >= 0) {
        /* ignore return value as we do not need to ensure writes
         * to /dev/null are successful */
        close(fd_dev_null);
        fd_dev_null = -1;
    }
}

struct event_array *event_array_new(void) {
    struct event_array *ev_arr = malloc(sizeof(*ev_arr));
    if (ev_arr == NULL) {
        REPORT_ERROR(strerror(errno));
        return NULL;
    }
    ev_arr->length = 0u;
    ev_arr->events = NULL;
    return ev_arr;
}

int event_array_append(struct event_array *ev_arr, struct event *ev) {
    if (++ev_arr->length == 1u) {
        ev_arr->events = malloc(sizeof(*ev_arr->events));
        if (ev_arr->events == NULL) {
            REPORT_ERROR(strerror(errno));
            return -1;
        }
    }
    else {
        struct event **realloced_events = realloc(ev_arr->events,
                ev_arr->length * sizeof(*ev_arr->events));
        if (realloced_events == NULL) {
            REPORT_ERROR(strerror(errno));
            return -1;
        }
        ev_arr->events = realloced_events;
    }
    ev_arr->events[ev_arr->length - 1] = ev;
    return 0;
}

void event_array_free(struct event_array *ev_arr) {
    for (int i = 0; i < (int)ev_arr->length; i++) {
        event_free(ev_arr->events[i]);
    }
}

void sigint_handler(evutil_socket_t fd, short what, void *arg) {
    PRINT_DEBUG("\b\bHandling sigint...\n");
    struct event_base *eb = arg;
    if (event_base_loopbreak(eb) < 0) {
        REPORT_ERROR("Failed to break out of the event loop. Invoking nuclear option...");
        abort();
    }
}

void sigchld_handler(evutil_socket_t fd, short what, void *arg) {
    PRINT_DEBUG("killing child...\n");
    struct sigchld_args *sa = arg;
    /* Handler is only added back onto event queue after returning. If another
     * process exits while handler is running, handler will not be called.
     * So we loop until error (p == -1) or no processes have terminated (p == 0)
     */
    while (1) {
        int status;
        pid_t p = waitpid(-1, &status, WNOHANG);
        if (p == -1) {
            if (errno == ECHILD) {
                PRINT_DEBUG("Waited for a process to terminate, but all "
                            "child processes have already terminated.\n");
                return;
            }
            else {
                fprintf(stderr, "Got an unexpected error while waiting for "
                            "child to terminate: %s\n", strerror(errno));
            }

            break;
        }
        else if (p == 0) {
            PRINT_DEBUG("Waited for a process to terminate, but none are "
                        "finished. Exiting event handler...\n");
            break;
        }
        else if (WIFEXITED(status) || (WIFSIGNALED(status) && WTERMSIG(status) == 13)) {
            ++sa->n_children_exited;
            PRINT_DEBUG("%dth child process ended\n", sa->n_children_exited);
            struct p4_node *pn = find_node_by_pid(sa->pf, p);

            if (pn == NULL) {
                REPORT_ERROR("Failed to find a node which matching pid of "
                             "recently-closed child process");
                return;
            }

            if (pn->in_pipes && pipe_array_close(pn->in_pipes) < 0) {
                PRINT_DEBUG("Closing all incoming pipes to node %s failed: %s\n",
                        pn->id, strerror(errno));
            }

            pn->ended = 1;

            if (pn->out_pipes) {
                for (int i = 0; i < (int)pn->out_pipes->length; i++) {
                    struct pipe *out_pipe = pn->out_pipes->pipes[i];
                    if (out_pipe->write_fd_is_open == 1) {
                        int close_successful = close(out_pipe->write_fd);
                        if (close_successful < 0) {
                            PRINT_DEBUG("Closing outgoing pipe from node %s on edge %s failed: %s\n",
                                    pn->id, out_pipe->edge_ids[0], strerror(errno));
                        }
                        else if (close_successful == 0)
                            out_pipe->write_fd_is_open = 0;
                    }
                }
            }

            for (int j = 0; j < (int)sa->pf->edges->length; j++) {
                if (strcmp(sa->pf->edges->edges[j]->to, pn->id) == 0) {
                    PRINT_DEBUG("edge %s finished after splicing %ld bytes\n",
                           sa->pf->edges->edges[j]->id,
                           sa->pf->edges->edges[j]->bytes_spliced);
                }
            }

            if (sa->n_children_exited == (int)sa->pf->nodes->length) {
                event_base_loopexit(sa->eb, NULL);
            }
        }
        else if (WIFSIGNALED(status)) {
            PRINT_DEBUG("child was signaled by %d\n", WTERMSIG(status));
            break;
        }
        else {
            REPORT_ERROR("A child process did not exited with an error.\n");
            break;
        }
    }
}

int write_single(struct writable_ev_args *wea) {
    int got_eof = 0;
    struct pipe *to_pipe = wea->to_pipes->pipes[0];
    ssize_t bytes = splice(wea->from_pipe->read_fd,
                           NULL,
                           to_pipe->write_fd,
                           NULL,
                           MAX_BYTES_TO_SPLICE,
                           SPLICE_F_NONBLOCK);

    if (bytes < 0) {
        if (errno != EAGAIN) {
            REPORT_ERROR(strerror(errno));
            return -1;
        }
    }
    else if (bytes > 0) {
        **wea->bytes_spliced += bytes;
        to_pipe->bytes_written = (size_t)bytes;
    }
    else {
        got_eof = 1;
    }

    return got_eof;
}

int write_multiple(struct writable_ev_args *wea) {
    int i = wea->to_pipe_idx;
    struct pipe *to_pipe = wea->to_pipes->pipes[i];

    if (to_pipe->bytes_written == 0) {
        ssize_t bytes = tee(wea->from_pipe->read_fd,
                            to_pipe->write_fd,
                            MAX_BYTES_TO_SPLICE,
                            SPLICE_F_NONBLOCK);

        if (bytes < 0) {
            if (errno != EAGAIN) {
                REPORT_ERROR(strerror(errno));
                return -1;
            }
        }
        else if (bytes > 0) {
            to_pipe->bytes_written = (size_t)bytes;
            *wea->bytes_spliced[i] += bytes;
        }
    }

    if (wea->to_pipes->pipes[i]->bytes_written < *wea->bytes_safely_written) {
        *wea->bytes_safely_written = wea->to_pipes->pipes[i]->bytes_written;
    }

    to_pipe->visited = 1;
    return 0;
}

void writable_handler(evutil_socket_t fd, short what, void *arg) {
    struct writable_ev_args *wea = arg;
    int got_eof;
    int last_writable_handler = 1;

    if ((what & EV_WRITE) == 0) {
        return;
    }

    if (wea->to_pipes->length == 1u) {
        got_eof = write_single(wea);
    }
    else {
        // tee/splice algorithm based on answer in
        // https://stackoverflow.com/a/14200975
        write_multiple(wea);

        for (int i = 0; i < (int)wea->to_pipes->length; i++) {
            if (wea->to_pipes->pipes[i]->visited == 0) {
                /* Not all pipes' writable events have fired; do not yet
                 * splice to /dev/null or add readable event. */
                last_writable_handler = 0;
                break;
            }
        }

        if (last_writable_handler == 1) {
            /* If all pipes have been visited, then this is the last
             * writable_handler to fire. bytes_safely_written lists how many
             * bytes from the input pipe have been safely tee'd to ALL output
             * pipes, and can therefore safely be discarded to /dev/null. */
            ssize_t bytes = splice(wea->from_pipe->read_fd,
                                   NULL,
                                   fd_dev_null,
                                   NULL,
                                   *wea->bytes_safely_written,
                                   SPLICE_F_NONBLOCK);
            if (bytes < 0) {
                got_eof = 0;
                if (errno != EAGAIN) {
                    REPORT_ERROR(strerror(errno));
                    return;
                }
            }
            else if (bytes > 0) {
                for (int j = 0; j < (int)wea->to_pipes->length; j++) {
                    wea->to_pipes->pipes[j]->bytes_written -= bytes;
                }
                got_eof = 0;
            }
            else {
                got_eof = 1;
            }
        }
    }
    if (last_writable_handler == 1) {
        if (got_eof == 1) {
            struct pipe *from_pipe = wea->from_pipe;
            PRINT_DEBUG("Edge %s (and possibly others) got EOF; closing pipes...\n",
                        from_pipe->edge_ids[0]);
            if (from_pipe->read_fd_is_open == 1 && close(from_pipe->read_fd) == 0)
                from_pipe->read_fd_is_open = 0;
            for (int k = 0; k < (int)wea->to_pipes->length; k++) {
                struct pipe *to_pipe = wea->to_pipes->pipes[k];
                if (to_pipe->write_fd_is_open == 1 && close(to_pipe->write_fd) == 0)
                    to_pipe->write_fd_is_open = 0;
            }
        }
        else {
            event_add(wea->readable_event, NULL);
        }
    }
}

void readable_handler(evutil_socket_t fd, short what, void *arg) {
    struct readable_ev_args *rea = arg;
    if ((what & EV_READ) == 0) {
        return;
    }

    *rea->bytes_safely_written = SIZE_MAX;
    for (int i = 0; i < (int)rea->to_pipes->length; i++) {
        rea->to_pipes->pipes[i]->visited = 0;
    }
    for (int j = 0; j < (int)rea->writable_events->length; j++) {
        event_add(rea->writable_events->events[j], NULL);
    }
}

void stats_handler(evutil_socket_t fd, short what, void *arg) {
    struct stats_ev_args *sa = arg;
    struct p4_file *pf = sa->pf;
    create_stats_file(pf);
}
