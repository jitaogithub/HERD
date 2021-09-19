#!/bin/bash

NUM_NODE=10
NODE_PREFIX="node-"
EXP_NAME="taoji-105893"
PROJECT_EXT="wisr-PG0"
DOMAIN="utah.cloudlab.us"
SKIP_NODES=""
# MANUAL_ORDER="1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 17 16"

if [ -z "$MANUAL_ORDER" ] ; then
  i=1
  while [ $i -le $NUM_NODE ] ; do
    skip=0;
    if [ "$1" != "-a" ] && [ "$1" != "--all" ] ; then
      for node in $SKIP_NODES ; do
        if [ "$i" -eq "$node" ] ; then
          skip=1
          break
        fi
      done
    fi

    if [ $skip -eq 0 ] ; then
      echo "$NODE_PREFIX$i.$EXP_NAME.$PROJECT_EXT.$DOMAIN"
    fi
    
    let i=$i+1
  done
else
  for i in $MANUAL_ORDER ; do
    echo "$NODE_PREFIX$i.$EXP_NAME.$PROJECT_EXT.$DOMAIN"
  done
fi
