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

// Global variables for congestion control
int ssthresh = 64;
int window_size = 1;
int windowFloat = 0;
int slow_start = 1;
int dup_cnt = 0;

// Global variables for checking the end of file
int file_end = 0;
int sentLastAck = 0;

// Global variables for the packet arrays

// Global variables for the socket
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
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


    tcp_packet* pkt_arr[WINDOW_SIZE]; // Stores the packets that have been sent
    int seqno_arr[WINDOW_SIZE]; // Stores the seqno of packets that were sent
    int timer_running = 0; // indicates whether a timer has already been started
    int last_byte_sent = -1; // This indicates the last byte in seqno array that was sent
    // int break_loop = 0; //flag to break loop

    dup_cnt = 0;    //this tracks number of duplicate acks
    int sent_packet_cnt = 0; // Tracks number of in flight packets, should not exceed congestion window

    //initializing seqno_arr and pkt_arr
    for (int i = 0; i < WINDOW_SIZE; i++){
        seqno_arr[i] = -1;
        pkt_arr[i] = NULL;
    }

    int index; // This is the index of the array where the next packet will be stored
    do
    {
        while (sent_packet_cnt < window_size){

            for (index=0; index< WINDOW_SIZE; index++) {
                if (seqno_arr[index] == -1)
                    break;
            }
            if(index == WINDOW_SIZE) break;

            // Read the data from the file
            len = fread(buffer, 1, DATA_SIZE, fp);
            if ( len <= 0)
            {
                file_end = 1;
                if(len < 0 || sentLastAck == 1) break;
            }
            
            // Make the packet
            tcp_packet* packet_to_send = make_packet(len);
            memcpy(packet_to_send->data, buffer, len);
            packet_to_send->hdr.seqno = next_seqno;

            // Add the packet_to_send to the pkt_arr
            pkt_arr[index] = packet_to_send;
            seqno_arr[index] = next_seqno;

            // Check if timer has already started, if not, start it and move the current packet to sndpkt
            if (!timer_running) {
                start_timer();
                sndpkt = packet_to_send;
                timer_running = 1;
                send_base = next_seqno;
            }

            
            // Increment the seqno
            next_seqno += len; 
            last_byte_sent = next_seqno;

            // VLOG information
            VLOG(DEBUG, "Sending packet %d to %s", 
                    next_seqno, inet_ntoa(serveraddr.sin_addr));

            /*
            Sending the packet
            * If the sendto is called for the first time, the system will
            * will assign a random port number so that server can send its
            * response to the src port.
            */
            if(sendto(sockfd, packet_to_send, TCP_HDR_SIZE + get_data_size(packet_to_send), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
            
            if(file_end== 1)
                sentLastAck = 1;
            sent_packet_cnt += 1;
        }

      //Wait for ACK
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("recvfrom");
        }

        recvpkt = (tcp_packet *)buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        
        //if we receive an in order acknowledgement -- for step 1, this will always be the case
        if (recvpkt->hdr.ackno > send_base){
            // set the new send base to the ack received
            send_base = recvpkt->hdr.ackno;
            dup_cnt = 0;

            for (int i = 0; i < WINDOW_SIZE; i++){

                if (seqno_arr[i] < send_base && seqno_arr[i] != -1){
                    // Set the packet to NULL
                    // VLOG(INFO, "Freeing packet %d", pkt_arr[i]->hdr.seqno);
                    seqno_arr[i] = -1;
                    free(pkt_arr[i]);
                    sent_packet_cnt--;

                    //incrementing window size in congestion avoidance mode
                    if (!slow_start){
                        windowFloat++;

                        if (windowFloat == window_size){
                            window_size = ++window_size < WINDOW_SIZE ? window_size : WINDOW_SIZE;
                            windowFloat = 0;
                            gettimeofday(&timer.it_value, NULL);
                            fprintf(csvFile,"%ld, %d\n", timer.it_value.tv_sec, window_size);
                        }
                    }

                    //incrementing window in slow_start
                    else{
                        window_size = ++window_size < WINDOW_SIZE ? window_size : WINDOW_SIZE;
                        if (window_size == ssthresh){
                            slow_start = 0;
                        }
                        //write to csv
                        gettimeofday(&timer.it_value, NULL);
                        fprintf(csvFile,"%ld, %d\n", timer.it_value.tv_sec, window_size);
                    }

                }
            }

            // if not all packets in our window are acked
            if (last_byte_sent > send_base){
                for (int i = 0; i < WINDOW_SIZE; i++){
                    if (seqno_arr[i] == send_base){
                        sndpkt = pkt_arr[i];
                        send_base = seqno_arr[i];
                        start_timer();
                        break;
                    }
                }
            }
            // if not we set the timer running to 0 because we are going to refill the array
            else {
                stop_timer();
                // VLOG(INFO, "All packets in window have been acked");
                if (file_end == 1) {    //if end file, send empty packet to receiver to indicate end of file
                    VLOG(INFO, "End Of File has been reached");
                    sndpkt = make_packet(0);
                    sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                            (const struct sockaddr *)&serveraddr, serverlen);
                    break;
                }

                timer_running = 0;
            }
        }

        
        else{   /*handling duplicate acks*/
            dup_cnt++;
            if (dup_cnt == 3){  //resend if 3 duplicate acks
                ssthresh = window_size/2 > 2 ? window_size/2 : 2;  //setting ssthresh to half of window size and starting slow start over again
                window_size = 1;   //resetting window_size to 1 to restart slow start
                windowFloat = 0; 
                slow_start = 1; //setting flag to start slow start again
                
                if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }

                start_timer();
                dup_cnt = 0;    //set dup_cnt to 0 after resending lost packet

                // Write to csv
                gettimeofday(&timer.it_value, NULL);
                fprintf(csvFile,"%ld, %d\n", timer.it_value.tv_sec, window_size);
            }
            
        }
        

    } while(1);


    int i;
    for (i=0; i<WINDOW_SIZE;i++) {
        if (seqno_arr[i] != -1) {
            free(pkt_arr[i]);
        }
    }

    return 0;

}

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum

        if(sndpkt->hdr.data_size == 0 && next_seqno == 0 && file_end == 1)
        {
            VLOG(INFO, "Last packet is acked, exiting");
            // If the last packet is acked, then we don't need to resend
            exit(EXIT_SUCCESS);
        }

        // VLOG(INFO, "Timeout happened for packet %d, %d, %d, %d", sndpkt->hdr.data_size, next_seqno, send_base, file_end);
        ssthresh = window_size/2 > 2 ? window_size/2 : 2;
        window_size = 1;
        windowFloat = 0;
        slow_start = 1;
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
        dup_cnt = 0;

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