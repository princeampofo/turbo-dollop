#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"
#define RECV_BUFF_SIZE 256

/*
 * You ar required to change the implementation to support
 * window size greater than one.
 * In the currenlt implemenetation window size is one, hence we have
 * onlyt one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *sndpkt;


int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);
    int next_seqno=0;
    tcp_packet* recvbuff[RECV_BUFF_SIZE];;
    // int recvbuff_index = 0;
    
    int i;
    for ( i= 0; i < RECV_BUFF_SIZE; i++){
        recvbuff[i] = NULL;
    }

    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        //VLOG(DEBUG, "waiting from server \n");
    
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }

        recvpkt = (tcp_packet *) buffer;
        // Packet that we are going to store so they don't overlap
        tcp_packet* store_pkt = make_packet(recvpkt ->hdr.data_size);
        memcpy(store_pkt ->data, recvpkt->data, recvpkt ->hdr.data_size);
        store_pkt->hdr.seqno = recvpkt->hdr.seqno;
        store_pkt->hdr.data_size = recvpkt->hdr.data_size;
        store_pkt->hdr.ackno = recvpkt->hdr.ackno;
        store_pkt->hdr.ctr_flags = store_pkt->hdr.ctr_flags;

        assert(get_data_size(recvpkt) <= DATA_SIZE);
        if ( recvpkt->hdr.data_size == 0) {
            VLOG(INFO, "End Of File has been reached");
            fclose(fp);
            break;
        }

        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

        // Getting index to place in recvbuff
        int index = (recvpkt->hdr.seqno - next_seqno)/DATA_SIZE; // Get the index to put it in
        
        // Check if what we received is equal to the seqno we are expecting -- in order
        if (recvpkt->hdr.seqno == next_seqno) {  
            
            recvbuff[index] = store_pkt;
            int stop_index = index;

            // Write until we find a gap
            
            while (recvbuff[stop_index] != NULL && stop_index < RECV_BUFF_SIZE) {
                fseek(fp, recvbuff[stop_index]->hdr.seqno, SEEK_SET);
                fwrite(recvbuff[stop_index]->data, 1, recvbuff[stop_index]->hdr.data_size, fp);
                next_seqno = recvbuff[stop_index]->hdr.seqno + recvbuff[stop_index]->hdr.data_size; // Setting the new next_seq_no -- it is the last in order packet + the data_size
                free(recvbuff[stop_index]);
                stop_index++;
            }

            
            
            // Shifting the buffered elements
            int i;
            for (i = 0; i+stop_index < RECV_BUFF_SIZE; i++) {
                recvbuff[i] = recvbuff[i+stop_index]; // Shifting back the elements

            }
            for(i = RECV_BUFF_SIZE-stop_index; i < RECV_BUFF_SIZE; i++ ) {
                recvbuff[i] = NULL; // Setting the rest of the elements to null
            }

            // Send ack
            sndpkt = make_packet(0);
            // next_seqno = recvbuff[stop_index-1]->hdr.seqno + recvbuff[stop_index-1]->hdr.data_size; 
            sndpkt->hdr.ackno = next_seqno;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
        }
        

        // If it is greater, add to the buffer
        else if (recvpkt->hdr.seqno > next_seqno) {
            // add packet to rcv buff, out of order packet

            recvbuff[index] = store_pkt;

            // (dup ack) ack the nextseqno, to indicate we are still waiting ofr this
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = next_seqno;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }

        }

        // If it is less resend ack, it means the ack was lost
        else {
            // Already written, ack was not received, only need to resend ack, no need to buffer
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = next_seqno;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
        }
        
    }

    return 0;
}