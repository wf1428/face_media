/*
 * sr505_test.c
 *
 * Simple HC-SR505 userspace test application.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>


static volatile sig_atomic_t running = 1;


static void signal_handler(int sig)
{
    running = 0;
}


static void print_time(void)
{
    struct timespec ts;
    struct tm tm_info;
    char buffer[64];

    clock_gettime(
        CLOCK_REALTIME,
        &ts);

    localtime_r(
        &ts.tv_sec,
        &tm_info);

    strftime(
        buffer,
        sizeof(buffer),
        "%Y-%m-%d %H:%M:%S",
        &tm_info);

    printf(
        "[%s.%03ld] ",
        buffer,
        ts.tv_nsec / 1000000);
}


int main(int argc, char *argv[])
{
    const char *device =
        "/dev/sr505";

    int fd;
    struct pollfd pfd;

    int last_state = -1;


    if (argc >= 2)
        device = argv[1];


    signal(
        SIGINT,
        signal_handler);

    signal(
        SIGTERM,
        signal_handler);


    printf(
        "=====================================\n");

    printf(
        " HC-SR505 test application\n");

    printf(
        "=====================================\n");

    printf(
        "Device : %s\n",
        device);

    printf(
        "GPIO   : GPIO2_C6 / GPIO86\n");

    printf(
        "Press Ctrl+C to exit\n");

    printf(
        "=====================================\n\n");


    /*
     * O_NONBLOCK:
     *
     * poll() waits for event.
     * read() only reads when data is ready.
     */
    fd = open(
        device,
        O_RDONLY |
        O_NONBLOCK);

    if (fd < 0) {

        fprintf(
            stderr,
            "open %s failed: %s\n",
            device,
            strerror(errno));

        return 1;
    }


    pfd.fd = fd;

    pfd.events =
        POLLIN;

    pfd.revents = 0;


    while (running) {

        int ret;


        ret = poll(
                &pfd,
                1,
                1000);


        if (ret < 0) {

            if (errno == EINTR)
                continue;


            fprintf(
                stderr,
                "poll failed: %s\n",
                strerror(errno));

            break;
        }


        /*
         * Timeout
         */
        if (ret == 0)
            continue;


        if (pfd.revents & POLLIN) {

            uint8_t state;

            ssize_t n;


            n = read(
                    fd,
                    &state,
                    sizeof(state));


            if (n < 0) {

                if (errno == EAGAIN)
                    continue;


                fprintf(
                    stderr,
                    "read failed: %s\n",
                    strerror(errno));

                break;
            }


            if (n != sizeof(state)) {

                fprintf(
                    stderr,
                    "invalid read size: %ld\n",
                    (long)n);

                continue;
            }


            state = !!state;


            /*
             * First read:
             * print initial state.
             */
            if (last_state < 0) {

                print_time();

                printf(
                    "INITIAL OUT = %d",
                    state);


                if (state) {

                    printf(
                        "  [HIGH / ACTIVE]\n");

                } else {

                    printf(
                        "  [LOW / IDLE]\n");
                }


                last_state = state;

                continue;
            }


            /*
             * Ignore duplicate event.
             */
            if (state == last_state)
                continue;


            print_time();


            /*
             * 0 -> 1
             */
            if (last_state == 0 &&
                state == 1) {

                printf(
                    "OUT: 0 -> 1"
                    "  [MOTION DETECTED]\n");
            }

            /*
             * 1 -> 0
             */
            else if (
                last_state == 1 &&
                state == 0) {

                printf(
                    "OUT: 1 -> 0"
                    "  [MOTION END /"
                    " HARDWARE DELAY END]\n");
            }

            else {

                printf(
                    "OUT: %d -> %d\n",
                    last_state,
                    state);
            }


            last_state = state;
        }


        /*
         * Check device error
         */
        if (pfd.revents &
            (POLLERR |
             POLLHUP |
             POLLNVAL)) {

            fprintf(
                stderr,
                "device error,"
                " revents=0x%x\n",
                pfd.revents);

            break;
        }
    }


    printf(
        "\nExit SR505 test.\n");


    close(fd);

    return 0;
}


