#!/bin/sh


if [ $1 == "down" ]
then
 curl -s http://localhost/api/v1/set?video0.bitrate=12000
fi

wfb_tx_cmd 8000 set_radio -B40 -G0 -S0 -L1 -M1 -N0
sleep 0.2
echo  "&L04 &F22 CPU:&C &B temp:&T tx_low_signal $1 ..." > /tmp/MSPOSD.msg
sleep 0.2

if [ $1 == "up" ]
then
	curl -s http://localhost/api/v1/set?video0.bitrate=12000
fi

exit 1
