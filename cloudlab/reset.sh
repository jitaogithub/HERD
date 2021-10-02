#!/bin/bash

WORKSPACE="$HOME/HERD"
HOSTS=`$WORKSPACE/cloudlab/nodes.sh | tail -n +2`

i=0
for host in $HOSTS ; do
  echo "Cleaning up $host"
  ssh -o StrictHostKeyChecking=no $host \
    "sudo killall -SIGKILL main yama_starter server"
  let i=$i+1

  if [[ ! -z $1 ]]; then
    if [ $i -eq $1 ]; then
      break
    fi
  fi
done
# kill server at last or clients will fail
echo "Cleaning localhost"
sudo killall -SIGKILL main  yama_starter server