#!/bin/bash

make clean

cd yama
./clean.sh
./build.sh

cd ..
make
