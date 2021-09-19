#!/bin/bash

HOSTS=`./cloudlab/nodes.sh --all`

echo "Configuring public keys for first node"
i=0
for host in $HOSTS; do
  if [ $i -eq 0 ] ; then 
    ssh -o StrictHostKeyChecking=no $host "ssh-keygen"
    pkey=`ssh -o StrictHostKeyChecking=no $host "cat ~/.ssh/id_rsa.pub"`
    let i=$i+1
  fi
  ssh -o StrictHostKeyChecking=no $host "echo -e \"$pkey\" >> ~/.ssh/authorized_keys"
done

i=0
for host in $HOSTS ; do
  echo "Configuring $host"
  ssh -o StrictHostKeyChecking=no $host "tmux new-session -d -s config \"
    sudo apt-get update &&
    sudo apt install -y sysstat linuxptp &&

    wget http://www.mellanox.com/downloads/ofed/MLNX_OFED-5.0-2.1.8.0/MLNX_OFED_SRC-debian-5.0-2.1.8.0.tgz &&
    tar xzf MLNX_OFED_SRC-debian-5.0-2.1.8.0.tgz &&
    cd MLNX_OFED_SRC-5.0-2.1.8.0/ &&
    sudo ./install.pl &&

    sudo reboot\""

done

