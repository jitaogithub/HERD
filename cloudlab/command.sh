#!/bin/bash

WORKSPACE="$HOME/HERD"
HOSTS=`$WORKSPACE/cloudlab/nodes.sh --all`

echo "Executing command \"$1\""
for host in $HOSTS ; do
  echo "    on $host"
  ssh -o StrictHostKeyChecking=no $host "$1" &
done

wait