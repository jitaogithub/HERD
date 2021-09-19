#!/bin/bash

WORKSPACE="$HOME/HERD"
HOSTS=`$WORKSPACE/cloudlab/nodes.sh --all`

for host in $HOSTS ; do
  echo "Running ptp4l on $host"
  ssh -o StrictHostKeyChecking=no $host "sudo nohup ptp4l -2 -i enp65s0f0 -m > ptp4l.log 2>&1 &" &
done
wait

for host in $HOSTS ; do
  echo "Running phc2sys on $host"
  ssh -o StrictHostKeyChecking=no $host "sudo nohup phc2sys -s enp65s0f0 -w -m > phc2sys.log 2>&1 &" &
done
wait
