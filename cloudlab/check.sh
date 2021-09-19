#!/bin/bash

HOSTS=`./cloudlab/nodes.sh --all`

for host in $HOSTS ; do
  echo "Checking $host"
  ret=`ssh -o StrictHostKeyChecking=no $host "ib_read_lat 10.10.1.1 2>&1 | grep \"Did not detect devices\""`
  if [ ! -z "$ret" ]; then
    echo -ne "\nWARNING: $host might have a problem"
  fi
  echo -ne "\n"
done