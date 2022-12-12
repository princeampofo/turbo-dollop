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

// Global variables 
#define STDIN_FD    0
#define RETRY  120 //millisecond
#define WINDOW_SIZE 256 //size of window

// Global variables for checking the sequences
int next_seqno=0;
int send_base=0;
int timerStarted = 0; // indicates whether a timer has already been started
int lastSentSeqno = -1; // This indicates the last byte in seqno array that was sent
int packetsInFlight = 0; // Tracks number of in flight packets, should not exceed congestion window

// Global variables for congestion control
int ssthresh = 64;
int window_size = 1;
int windowFloat = 0;
int slow_start = 1;
int dupAcks = 0;  //this tracks number of duplicate acks


// Global variables for checking the end of file
int endOfFile = 0;
int sentLastAck = 0;

// Global variables for the packet arrays
tcp_packet* packetsArray[WINDOW_SIZE];  // Stores the packets that are currently in our window
int seqnoArray[WINDOW_SIZE];            // Stores the seqno of packets cos packetsArray messes up when freeing memory 

// Global variables for the socket
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;                     // Use to keep track of the base packet in the window 
tcp_packet *recvpkt;
sigset_t sigmask;       


FILE *csvFile; // The csv file we're saving to

// Function prototypes
void resend_packets(int sig);
void start_timer();
void stop_timer();
void init_timer(int delay, void (*sig_handler)(int));


int main (int argc, char **argv)
{
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    //initializing seqnoArray and packetsArray
    for (int i = 0; i < WINDOW_SIZE; i++){
        packetsArray[i] = NULL;
        seqnoArray[i] = -1;
    }

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    //creating csv file to keep track of window_size changes
    csvFile = fopen("window_size_receiver.csv", "w");
    if (csvFile == NULL) 
    { 
        printf("Could not open receiver file"); 
        return 0; 
    } 

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    init_timer(RETRY, resend_packets);

    int index; // This is the index of the array where the next packet will be stored
    while(1){
        while (packetsInFlight < window_size){

            for (index=0; index< WINDOW_SIZE; index++) {
                if (seqnoArray[index] == -1)
                    break;
            }
            if(index == WINDOW_SIZE) break;

            // Read the data from the file
            len = fread(buffer, 1, DATA_SIZE, fp);
            if ( len <= 0)
            {
                endOfFile = 1;
                // If we have already sent the last ack, break and wait for timeout to terminate
                // This is to ensure that the last ack was really received by the receiver
                if(len < 0 || sentLastAck == 1) break;      
            }
            
            // Make the packet
            tcp_packet* tempPacket = make_packet(len);
            memcpy(tempPacket->data, buffer, len);
            tempPacket->hdr.seqno = next_seqno;

            // Add the tempPacket to the packetsArray
            packetsArray[index] = tempPacket;
            seqnoArray[index] = tempPacket->hdr.seqno;

            next_seqno += len; 
            lastSentSeqno = next_seqno;

            // If this is the start of a new window, start the timer and keep track of the base packet
            if (!timerStarted) {
                start_timer();
                sndpkt = tempPacket;
                timerStarted = 1;
                send_base = tempPacket->hdr.seqno;
            }

            // VLOG information
            VLOG(DEBUG, "Sending packet %d to %s", 
                    next_seqno, inet_ntoa(serveraddr.sin_addr));

            /*
            Sending the packet
            * If the sendto is called for the first time, the system will
            * will assign a random port number so that server can send its
            * response to the src port.
            */
            if(sendto(sockfd, tempPacket, TCP_HDR_SIZE + get_data_size(tempPacket), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("Sending packet failed in while loop");
            }
            
            if(endOfFile== 1)
                sentLastAck = 1;

            packetsInFlight += 1;
        }

        //Wait for ACK
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("recvfrom");
        }

        recvpkt = (tcp_packet *)buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        if(recvpkt->hdr.ackno <= send_base){   /*handling duplicate acks*/
            if (++dupAcks == 3){  
                stop_timer();                   // Stop the timer
                timerStarted = 0;
                resend_packets(SIGALRM);            // Trigger resend by sending SIGALRM
            }
            continue;
        }

        // If new ack is above send base then we can ack all the packets below it and move the send base
        send_base = recvpkt->hdr.ackno;
        dupAcks = 0;

        for (int i = 0; i < WINDOW_SIZE; i++){
            if (seqnoArray[i] < send_base && seqnoArray[i] != -1){
                // Set the packet to NULL
                // VLOG(INFO, "Freeing packet %d", packetsArray[i]->hdr.seqno);
                seqnoArray[i] = -1;
                free(packetsArray[i]);
                packetsInFlight--;

                //When not in slow start, increment via congestion avoidance otherwise increment window_size by 1
                if (!slow_start){
                    if (++windowFloat == window_size){
                        window_size = ++window_size < WINDOW_SIZE ? window_size : WINDOW_SIZE;
                        windowFloat = 0;
                    }
                }
                else{
                    window_size = ++window_size < WINDOW_SIZE ? window_size : WINDOW_SIZE;
                    if (window_size == ssthresh){    //if window size is equal to ssthresh, exit slow start
                        slow_start = 0;
                    }
                }
                gettimeofday(&timer.it_value, NULL);
                fprintf(csvFile,"%ld, %d\n", timer.it_value.tv_sec, window_size);
            }
        }
        
        // If the last packet was sent and the ack is for it, stop the timer
        if(lastSentSeqno <= send_base){
            if(endOfFile == 1){
                timerStarted = 1;
                sndpkt = make_packet(0);
            }else{
                stop_timer();
                timerStarted = 0;
            }
            continue;
        }

        // Find a new packet to be the base packet for the window
        for (int i = 0; i < WINDOW_SIZE; i++){
            if (seqnoArray[i] == send_base){
                send_base = seqnoArray[i];
                sndpkt = packetsArray[i];
                start_timer();
                timerStarted = 1;
                break;
            }
        }

    }

    // Free all the packets in the packetsArray
    for (int i=0; i<WINDOW_SIZE;i++) {
        if (seqnoArray[i] != -1) {
            free(packetsArray[i]);
        }
    }

    return 0;
}

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Timeout happened for packet %d, %d, %d, %d", sndpkt->hdr.data_size, next_seqno, send_base, endOfFile);
        if(sndpkt->hdr.data_size == 0 && next_seqno == send_base && endOfFile == 1)
        {
            VLOG(INFO, "Last packet is acked, exiting");
            // If the last packet is acked, then we don't need to resend
            exit(EXIT_SUCCESS);
        }

        // If the last packet is not acked, then we need to resend
        //Resend the base packet of the congestion window and start a chain that sends all packets in the window

        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("Error in sending packet in timeout");
        }
        
        slow_start = 1;
        windowFloat = 0;
        ssthresh = window_size/2 > 2 ? window_size/2 : 2;
        window_size = 1;
        

        dupAcks = 0;
        start_timer();
        timerStarted = 1;

        // Write to csv
        gettimeofday(&timer.it_value, NULL);
        fprintf(csvFile, "%ld, %d\n", timer.it_value.tv_sec, window_size);
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
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}