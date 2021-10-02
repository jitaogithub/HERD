#!/bin/bash

WORKSPACE="$HOME/HERD"

HOSTS=`$WORKSPACE/cloudlab/nodes.sh | tail -n +2`
SUBNET=10.10.1
BASE_HOST_ID=1
BASE_PORT=18515
HRD_BASE_PORT=5500

NUM_ENTITIES=3
WEIGHTS=(1 1 1)
NUM_SERVERS=(3 3 3)
NUM_CLIENT_HOSTS=9
NUM_CLIENTS_PER_HOST=(3 3 3)

RUN_BASELINE=1
RUN_DYNAMIC=0

if [ $RUN_BASELINE -eq "1" ] ; then
  # server configuration
  conf="0 0 0 0 ${NUM_ENTITIES}"
  srv_port=${HRD_BASE_PORT}
  for ((i=0; i<${NUM_ENTITIES}; i++)) ; do
    conf="${conf} ${NUM_SERVERS[$i]}"
    for ((j=0; j<${NUM_SERVERS[$i]}; j++)) ; do
      conf="${conf} ${srv_port}"
      let srv_port=${srv_port}+1
    done

    num_clients=`echo "${NUM_CLIENT_HOSTS} * ${NUM_CLIENTS_PER_HOST[$i]}" | bc -l`
    conf="${conf} ${num_clients}"
    for ((j=0; j<${num_clients}; j++)) ; do
      conf="${conf} 1,${WEIGHTS[$i]},6666,10.10.1.254" # port and ip are fake
    done
  done

  # run server
  sudo ipcrm -a
  sudo IBDEV_NAME=mlx5_3 ./yama_starter ${conf} & #  > server.log 2>&1 &
  echo "Server on localhost started with conf: ${conf}"

  sleep 10s

  clt_base_conf="1 \${base_id} 0 0 ${NUM_ENTITIES}"
  srv_port=${HRD_BASE_PORT}
  for ((i=0; i<${NUM_ENTITIES}; i++)) ; do
    clt_base_conf="${clt_base_conf} ${NUM_CLIENTS_PER_HOST[$i]} ${NUM_SERVERS[$i]}"
    for ((j=0; j<${NUM_SERVERS[$i]}; j++)) ; do
      clt_base_conf="${clt_base_conf} 1,${WEIGHTS[$i]},${srv_port},${SUBNET}.${BASE_HOST_ID}"
      let srv_port=${srv_port}+1
    done
  done

  sleep 2s

  j=0
  base_id=0
  for host in ${HOSTS} ; do
    if [ $j -ge ${NUM_CLIENT_HOSTS} ] ; then 
      break
    fi
    conf=`eval echo ${clt_base_conf}`

    ssh -oStrictHostKeyChecking=no $host "cd ${WORKSPACE}; sudo IBDEV_NAME=mlx5_3 nohup ./yama_starter ${conf} > client.log 2>&1 &"
    echo "Client on ${host} started with conf: ${conf}"

    let j=${j}+1
    let base_id=${base_id}+${NUM_CLIENTS_PER_HOST[0]}
  done

fi


if [ $RUN_DYNAMIC -eq "1" ] ; then
  # server configuration
  conf="0 0 0 0 ${NUM_ENTITIES}"
  srv_port=${HRD_BASE_PORT}
  for ((i=0; i<${NUM_ENTITIES}; i++)) ; do
    conf="${conf} ${NUM_SERVERS[$i]}"
    for ((j=0; j<${NUM_SERVERS[$i]}; j++)) ; do
      conf="${conf} ${srv_port}"
      let srv_port=${srv_port}+1
    done

    num_clients=`echo "${NUM_CLIENT_HOSTS} * ${NUM_CLIENTS_PER_HOST[$i]}" | bc -l`
    conf="${conf} ${num_clients}"
    for ((j=0; j<${num_clients}; j++)) ; do
      conf="${conf} 1,${WEIGHTS[$i]},6666,10.10.1.254" # port and ip are fake
    done
  done

  # conf="${conf} ${NUM_CLIENT_HOSTS}"
  # let host_id=${BASE_HOST_ID}+1
  # for ((i=0; i<${NUM_CLIENT_HOSTS} ; i++)) ; do 
  #   conf="${conf} 18515,${host_id}"
  #   let host_id=${host_id}+1
  # done

  # run server
  sudo ./yama/server 18515 &
  sudo ipcrm -a
  sudo IBDEV_NAME=mlx5_3 ./yama_starter ${conf} & #  > server.log 2>&1 &
  echo "Server on localhost started with conf: ${conf}"

  sleep 15s

  clt_base_conf="1 \${base_id} 1 0 ${NUM_ENTITIES}"
  srv_port=${HRD_BASE_PORT}
  for ((i=0; i<${NUM_ENTITIES}; i++)) ; do
    clt_base_conf="${clt_base_conf} ${NUM_CLIENTS_PER_HOST[$i]} ${NUM_SERVERS[$i]}"
    for ((j=0; j<${NUM_SERVERS[$i]}; j++)) ; do
      clt_base_conf="${clt_base_conf} 1,${WEIGHTS[$i]},${srv_port},${SUBNET}.${BASE_HOST_ID}"
      let srv_port=${srv_port}+1
    done
  done
  
  clt_base_conf="${clt_base_conf} 1 18515,10.10.1.1"

  j=0
  base_id=0
  for host in ${HOSTS} ; do
    if [ $j -ge ${NUM_CLIENT_HOSTS} ] ; then 
      break
    fi
    conf=`eval echo ${clt_base_conf}`

    ssh -oStrictHostKeyChecking=no $host "cd ${WORKSPACE}; sudo IBDEV_NAME=mlx5_3 nohup ./yama_starter ${conf} > client.log 2>&1 &"
    echo "Client on ${host} started with conf: ${conf}"

    let j=${j}+1
    let base_id=${base_id}+${NUM_CLIENTS_PER_HOST[0]}
  done

fi