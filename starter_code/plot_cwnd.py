
import numpy as np
import matplotlib.pyplot as plt
from argparse import ArgumentParser

parser = ArgumentParser(description="plot")

parser.add_argument('--dir', '-d',
                    help="Directory to store outputs",
                    required=True)

parser.add_argument('--name', '-n',
                    help="name of the experiment(has to be in the same directory as the output file)",
                    required=True)

# argumument parser
args = parser.parse_args()

# create a figure
fig = plt.figure(figsize=(21,3), facecolor='w')
ax = plt.gca()

# store cwnd and time values
throughputDL = []
timeDL = []

# open the file
traceDL = open (args.dir+"/"+str(args.name), 'r')

cnt = 0

# while not the end of the file
while True:
    # read a line
    line = traceDL.readline()
    # if line is empty, you are done with the file
    if not line:
        break
    # split the line into a list called "fields" using "," as the delimiter
    fields = line.strip().split(",")
    # the second field is the cwnd value
    cwnd = int(fields[1])
    # append the cwnd value to the throughputDL list
    throughputDL.append(cwnd)
    # append the time value to the timeDL list
    timeDL.append(cnt)
    # increment the time by one
    cnt += 1

# close the file
traceDL.close()

# plot the throughputDL list vs. the timeDL list
plt.plot(timeDL, throughputDL, lw=2, color='r')
plt.ylabel("CWND")
plt.xlabel("CWND Change Count(Update Interval: 1)")
plt.xlim([0,400])
plt.grid(True, which="both")
plt.savefig(args.dir+'/cwnd_plot1.pdf',dpi=1000,bbox_inches='tight')
