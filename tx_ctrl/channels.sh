#!/bin/sh
echo "Entering script"

if [ $1 -eq 9 ]
then
	echo "Entering channel9"
		
	if [ $2 -lt 1150 ]
	then
		kill -9 $(pgrep tunnel_rx)
		#echo -n 'set mode pid' | nc 127.0.0.1 9995
		tunnel_rx -p 160 -u 5800 -K /etc/drone.key -i 7669206 wlan0 2>&1 | tx_ctrl --wlanid wlan0 --card-type rtl8733bu --alink &
		sleep 1
		echo  "&L04 &F22 CPU:&C &B temp:&T Starting tx_ctrl ..." > /tmp/MSPOSD.msg
		
	elif [ $2 -gt 1150 ] && [ $2 -lt 1250 ]
	then
		20mhz-mcs1
	elif [ $2 -gt 1350 ] && [ $2 -lt 1450 ]
	then
		20mhz-mcs2
	elif [ $2 -gt 1550 ] && [ $2 -lt 1650 ]
	then
		40mhz-mcs1
	elif [ $2 -gt 1750 ] && [ $2 -lt 1850 ]
	then
		40mhz-mcs2
	elif [ $2 -gt 1950 ]
	then
		#reset tx_ctrl
		#kill -SIGUSR2 $(pgrep tx_ctrl)
		/etc/init.d/S98datalink stop
		sleep 0.5
		/etc/init.d/S98datalink start
		sleep 0.5
		/etc/init.d/S95majestic restart
		sleep 2
		echo  "&L04 &F22 CPU:&C &B temp:&T Starting tx_ctrl ..." > /tmp/MSPOSD.msg
		sleep 0.2
		wfb_tx_cmd 8000 set_radio -B20 -G0 -S0 -L1 -M1 -N0
	fi
fi

exit 1
