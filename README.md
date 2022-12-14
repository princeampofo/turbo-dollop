# TCP Project

## Description
We implemented a simple TCP from the ground up by developer receiver and sender protocols with reliable data transfer and congestion control implementations.

## Implementation

### Sender
We have a congestion window that determines how mnay packets we can send at any time. 
We created an array to store packets that are in flight. The packets are removed once they are acked. As per the project's objectives, we created a csv file named *window_size_receiver.csv* to keep track of changes in congestion window size. We use this to plot our graphs later. 

### Receiver
At the receiving end, all in-order packets are written to the file otherwise they are buffered in an receiver array. We sent back duplicate acks for in order packets.

### MakeFile
To compile the program


## Graphs
The pdf graphs in the repository were created by experimenting with the following trace file and mahi mahi parameters:
```
mm-delay 5 mm-loss uplink 0.2 mm-link --meter-all /home/.../cellularGold /home/.../cellularGold
```
To plot throughput, run the following command:
```
python3 plot_script_wireshark.py --dir <directory_name>
```
The file has to be named **throughput_receiver.csv**

To plot congestion window size changes, run the following command:
```
python3  plot_script_cwnd.py --dir <directory_name>
```
The file has to be named **cwnd_receiver.csv**

