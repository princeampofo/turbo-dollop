#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include "packet.h"
#include "common.h"

#define STDIN_FD 0
#define RETRY 120 // milli second
#define WINDOW_SIZE 256

int next_seqno = 0;
int send_base = 0;
int window_size = 10;
int ssthresh = 64;
int cwnd = 1;
int cwnd_float = 0;
int slow_start = 1;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;
int dupAcks = 0;

FILE *csv_file;    // The csv file we're saving to
struct timeval tp; // The timevalue

int timerStarted = 0;    // indicates whether a timer has already been started
int lastSeqno = -1;      // This indicates the last byte in seqno array that was sent
int endOfFile = 0;       // chck if file has finished
int packetsInFlight = 0; // Tracks number of in flight packets, should not exceed congestion window

tcp_packet *packetArr[WINDOW_SIZE]; // Stores the packets that have been sent
int seqnoArr[WINDOW_SIZE];          // Stores the seqno of packets that were sent

void resend_packets(int sig);
void start_timer();
void stop_timer();
void init_timer(int delay, void (*sig_handler)(int));

// Self defined functions

// Checks whether there is an empty space in the array, returns the first one if there is

int main(int argc, char **argv)
{
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;
    /* check command line arguments */
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL)
    {
        error(argv[3]);
    }

    // creating csv file to keep track of cwnd changes
    csv_file = fopen("cwnd_receiver.csv", "w");
    if (csv_file == NULL)
    {
        printf("Could not open receiver file");
        return 0;
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    /* initialize server server details */
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0)
    {
        fprintf(stderr, "ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    init_timer(RETRY, resend_packets);

    // This is the actual implementatio

    // dupAcks = 0;             // this tracks number of duplicate acks

    // initializing seqnoArr and packetArr
    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        seqnoArr[i] = -1;
        packetArr[i] = NULL;
    }

    while (1)
    {
        while (packetsInFlight < cwnd)
        {
            int index;
            for (index = 0; index < WINDOW_SIZE; index++)
            {
                if (seqnoArr[index] == -1)
                {
                    break;
                }
            }
            if (index == WINDOW_SIZE)
                break;

            // Read the data from the file
            len = fread(buffer, 1, DATA_SIZE, fp);
            // if end of file
            if (len <= 0)
            {
                endOfFile = 1;
                break;
            }

            // Make the packet
            tcp_packet *tempPkt = make_packet(len);
            memcpy(tempPkt->data, buffer, len);
            tempPkt->hdr.seqno = next_seqno;

            // Increment the seqno
            next_seqno += len;
            lastSeqno = next_seqno;

            // Add the tempPkt to the packetArr
            packetArr[index] = tempPkt;
            seqnoArr[index] = tempPkt->hdr.seqno;

            // Check if timer has already started, if not, start it and move the current packet to sndpkt
            if (!timerStarted)
            {
                start_timer();
                sndpkt = tempPkt;
                timerStarted = 1;
                send_base = tempPkt->hdr.seqno;
            }

            // // VLOG information
            // VLOG(DEBUG, "Sending packet %d to %s",
            //         next_seqno, inet_ntoa(serveraddr.sin_addr));

            if (sendto(sockfd, tempPkt, TCP_HDR_SIZE + get_data_size(tempPkt), 0,
                       (const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("Sending file failed in the inner while loop");
            }

            packetsInFlight += 1;
        }

        // Wait for ACK
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                     (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("Error receiving ack");
        }

        recvpkt = (tcp_packet *)buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        // if we receive an in order acknowledgement -- for step 1, this will always be the case
        if (recvpkt->hdr.ackno > send_base)
        {
            // set the new send base to the ack received
            send_base = recvpkt->hdr.ackno;
            dupAcks = 0;
            // free up space in the effective window
            for (int i = 0; i < WINDOW_SIZE; i++)
            {
                if (seqnoArr[i] < send_base && seqnoArr[i] != -1)
                {
                    seqnoArr[i] = -1;   // setting it to -1 because seqno can start with 0
                    free(packetArr[i]); // freeing packets that have been acked
                    packetsInFlight--;

                    // incrementing window size in congestion avoidance mode
                    if (!slow_start)
                    {
                        if (++cwnd_float == cwnd)
                        {
                            cwnd = ++cwnd < WINDOW_SIZE ? cwnd : WINDOW_SIZE;
                            cwnd_float = 0;
                            gettimeofday(&tp, NULL);
                            fprintf(csv_file, "%ld, %d\n", tp.tv_sec, cwnd);
                        }
                    }
                    else
                    {
                        cwnd = ++cwnd < WINDOW_SIZE ? cwnd : WINDOW_SIZE;
                        if (cwnd == ssthresh)
                        {
                            slow_start = 0;
                        }

                        // write to csv
                        gettimeofday(&tp, NULL);
                        fprintf(csv_file, "%ld, %d\n", tp.tv_sec, cwnd);
                    }
                }
            }

            // if not all packets in our window are acked
            if (lastSeqno > send_base)
            {
                for (int i = 0; i < WINDOW_SIZE; i++)
                {
                    if (seqnoArr[i] == send_base)
                    {
                        sndpkt = packetArr[i];
                        send_base = seqnoArr[i];
                        start_timer();
                        break;
                    }
                }
            }
            // if not we set the timer running to 0 because we are going to refill the array
            else
            {
                stop_timer();
                timerStarted = 0;
                if (endOfFile == 1)
                {
                    VLOG(INFO, "End Of File has been reached");
                    sndpkt = make_packet(0);
                    sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                           (const struct sockaddr *)&serveraddr, serverlen);
                    break;
                }
            }
        }

        else
        {
            if (++dupAcks == 3)
            { // resend if 3 duplicate acks
                // ssthresh = cwnd / 2 > 2 ? cwnd / 2 : 2; // setting ssthresh to half of window size and starting slow start over again
                // cwnd = 1;                               // resetting cwnd to 1 to restart slow start
                // cwnd_float = 0;
                // slow_start = 1; // setting flag to start slow start again

                // if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,
                //            (const struct sockaddr *)&serveraddr, serverlen) < 0)
                // {
                //     error("sendto");
                // }

                // start_timer();
                // dupAcks = 0; // set dupAcks to 0 after resending lost packet

                // // Write to csv
                // gettimeofday(&tp, NULL);
                // fprintf(csv_file, "%ld, %d\n", tp.tv_sec, cwnd);
                resend_packets(SIGALRM);
                start_timer();
            }
        }
    }

    // freeing packets before exiting program
    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        if (seqnoArr[i] != -1)
        {
            free(packetArr[i]);
        }
    }

    return 0;
}

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        // Resend all packets range between
        // sendBase and nextSeqNum
        VLOG(INFO, "Timeout happened");
        ssthresh = cwnd / 2 > 2 ? cwnd / 2 : 2;
        cwnd = 1;
        cwnd_float = 0;
        slow_start = 1;
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,
                   (const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("Failed send to in Timeout");
        }
        dupAcks = 0;

        // Write to csv
        gettimeofday(&tp, NULL);
        fprintf(csv_file, "%ld, %d\n", tp.tv_sec, cwnd);
    }
}

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

/*
 * init_timer: Initialize timeer
 * delay: delay in milli seconds
 * sig_handler: signal handler function for resending unacknoledge packets
 */
void init_timer(int delay, void (*sig_handler)(int))
{
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000; // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;
    timer.it_value.tv_sec = delay / 1000; // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}