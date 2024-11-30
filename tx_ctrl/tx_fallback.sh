#!/bin/sh

curl -s http://localhost/api/v1/set?video0.bitrate=7000
sleep 0.2
wfb_tx_cmd 8000 set_radio -B20 -G0 -S0 -L1 -M1 -N0
sleep 0.2
echo  "&L04 &F22 CPU:&C &B temp:&T fallback ..." > /tmp/MSPOSD.msg
sleep 0.2

exit 1