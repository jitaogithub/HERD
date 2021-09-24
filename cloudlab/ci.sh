#!/bin/bash

HOSTS=`./cloudlab/nodes.sh --all`

TARBALL=HERD.tar.gz
PROJECT_DIRNAME=HERD

tar -czf $TARBALL scripts/ YCSB/ yama/ ./*

for host in $HOSTS ; do
  echo "Pushing to $host ..."
  scp -rq -o StrictHostKeyChecking=no $TARBALL $host:~/ > /dev/null 2>&1 &
done
wait

for host in $HOSTS ; do
  echo "Building on $host ..."
  ssh -o StrictHostKeyChecking=no $host "mkdir -p $PROJECT_DIRNAME; tar -xzf $TARBALL -C $PROJECT_DIRNAME; cd $PROJECT_DIRNAME; ./do.sh > build.log 2>&1" > /dev/null 2>&1 &
done
wait

rm -f $TARBALL
echo "Done"
