# TCP Project

## Description

We implemented a simple TCP from the ground up by developer receiver and sender protocols with reliable data transfer and congestion control implementations.

## Implementation

### Sender

We have a congestion window that determines how mnay packets we can send at any time.
We created an array to store packets that are in flight. The packets are removed once they are acked. As per the project's objectives, we created a csv file named _CWND.csv_ to keep track of changes in congestion window size. We use this to plot our graphs later.

#### Reliable data transfer

Out of order packets are held in the receiver's buffer, and each one receives a duplicate acknowledgement.<br>
Order packets contain cumulative acknowledgements for each order.<br>
To make sure that lost packets are retransmitted, each sent packet has a timeout.<br>
A retransmission also happens after receiving three duplicate acknowledgements, or three out of order packets.<br>

#### Congestion Control

For flow control, we send the min(cwnd, rwnd) packets to the receiver.

#### Slow Start

In slow start, the window is increased by 1 after each packet for which we obtain an ACK.<br>
Once we cross the threshold, we begin to move slowly.<br>
We count a timeout or three duplicate acknowledgements as a packet loss, set cwnd to 1, and restart slow start in response.<br>

#### Congestion Avoidance

We enter this phase after we have crossed ssthresh.<br/>
We increment by 1/cwnd on every received ack.<br/>
If we get a loss, we set cwnd to 1 and enter slow start.

### Receiver

At the receiving end, all in-order packets are written to the file otherwise they are buffered in an receiver array. We sent back duplicate acks for in order packets.

### MakeFile

To compile the program

## Graphs

The parameters we used to spawn a shell with mahimahi

## How to run the program

Run makefile

```
cd startercode
make
```

Run the client
`cd obj && ./rdt_receiver <port> <filename>`

Run the sender
`cd obj && ./rdt_sender <hostname> <port> <filename>`

**It is important to make sure the receiver filename is different from the senders.**
