#!/bin/sh

while true
do
    ./dac_test /dev/spidev0.0 0
    usleep 100000

    ./dac_test /dev/spidev0.0 1023
    usleep 100000
done
