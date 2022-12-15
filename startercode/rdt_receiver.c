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

#define RCV_WND_SIZE 128

tcp_packet *recvpkt;
tcp_packet *sndpkt;

void endOfFileMethod(int sock, struct sockaddr_in clientaddr, int clientlen, tcp_packet *rcvpkt);
void sendAck(int sock, struct sockaddr_in clientaddr, int clientlen, int next_seqno);

/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
tcp_packet *rcvpkt;
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
    int next_seqno = 0;

    tcp_packet *arrayOfrcvpkts[RCV_WND_SIZE];

    // Initialize the array of packets to null
    int j = 0;
    while (j < RCV_WND_SIZE){
        arrayOfrcvpkts[j] = NULL;
        j++;
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

        // assert that size of packet is less than or equal to data size
        assert(get_data_size(recvpkt)<=DATA_SIZE);

        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

        // if the packet size is zero we have reached end of file
        if (get_data_size(recvpkt) == 0){
            endOfFileMethod(sockfd, clientaddr, clientlen, recvpkt);
            // close the file
            fclose(fp);
            break;
        }

        // get the index in the arrayrcvpkts where the packet should be stored
        int p_ind_in_recv_buffer = (recvpkt->hdr.seqno - next_seqno)/DATA_SIZE;

        // if the packet matches the expected sequence number
        if (recvpkt->hdr.seqno == next_seqno){
            // buffer the packet
            arrayOfrcvpkts[p_ind_in_recv_buffer] = recvpkt;

            // write to file after all the packets in the window have been received
            int i = 0;
            while (i < RCV_WND_SIZE){
                if (arrayOfrcvpkts[i] != NULL){
                    // seek to the correct position in the file
                    fseek(fp, arrayOfrcvpkts[i]->hdr.seqno, SEEK_SET);

                    // write the data to the file
                    fwrite(arrayOfrcvpkts[i]->data, 1, arrayOfrcvpkts[i]->hdr.data_size, fp);

                    // update the next_seqno
                    next_seqno += arrayOfrcvpkts[i]->hdr.data_size;

                    // free the memory
                    arrayOfrcvpkts[i] = NULL;
                }
                else{
                    break;
                }
                i++;
            }

            // if there are any buffered packets that weren't written to file, shift them to the beginning of the array
            if (i < RCV_WND_SIZE){
                int k = 0;
                while (i < RCV_WND_SIZE){
                    arrayOfrcvpkts[k] = arrayOfrcvpkts[i];
                    arrayOfrcvpkts[i] = NULL;
                    i++;
                    k++;
                }
            }

            // send the Ack by calling the sendAck method
            sendAck(sockfd, clientaddr, clientlen, next_seqno);

        }

        // if the packet is out of order
        else if (recvpkt->hdr.seqno > next_seqno){
            // buffer the packet
            arrayOfrcvpkts[p_ind_in_recv_buffer] = recvpkt;

            // print out of order message
            VLOG(INFO, "Packet out of order");
            // send the ACK
            sendAck(sockfd, clientaddr, clientlen, next_seqno);
        }

        // if the packet is a duplicate
        else if (recvpkt->hdr.seqno < next_seqno){

            // print duplicate message
            VLOG(INFO, "Duplicate packet received");
            // send the ACK
            sendAck(sockfd, clientaddr, clientlen, next_seqno);
        }

    }
        

    return 0;
}


void endOfFileMethod(int sock, struct sockaddr_in clientaddr, int clientlen, tcp_packet *rcvpkt){
    VLOG(INFO, "End Of File has been reached");

    // send the last ACK 100 times(a hack to circumvent the loss of the last ACK)
    // this is a hack to make sure the client has received the last packet and is not waiting for more
    // since we are breaking out of the loop
    for (int i = 0; i < 100;i++){ 
        sndpkt = make_packet(0);
        sndpkt->hdr.ackno = rcvpkt->hdr.seqno;
        // printf the sequence number of the last packet
        sndpkt->hdr.ctr_flags = ACK;
        if(sendto(sock, sndpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)&clientaddr, clientlen) < 0) {
            error("Could not send ACK\n");
        }
    } 	
}

void sendAck(int sockfd, struct sockaddr_in clientaddr, int clientlen, int next_seqno){
    sndpkt = make_packet(0);
    sndpkt->hdr.ackno = next_seqno;
    sndpkt->hdr.ctr_flags = ACK;
    if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
        error("ERROR sending ACK");
    }
}