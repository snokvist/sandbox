## Introduction
I was asked to try out the new docker for Anjoy SSC338q which i already owned, and have been using extensively before.

The package was well put together like with Mario AIO and everything was well protected
![20241122_192825](https://github.com/user-attachments/assets/ab6b6d18-5638-4805-8479-fa440d676f92)

The Docker gives a good impression and looks and feel premium at first glance!
![20241122_201104](https://github.com/user-attachments/assets/52261a7e-ba37-482d-9323-94e8f27565e5)
![20241123_072448](https://github.com/user-attachments/assets/634d1651-8c7e-49bc-83a1-13193976545a)

The XT30 holes are too small for my XT30 connectors to fit. The spacings looks right, but the holes are too small. I will mount a pigtail instead. I also add a heatsink for RTL8733bu since they run quite hot.
![20241122_222357](https://github.com/user-attachments/assets/0cc3e7c3-fda6-4319-bbd7-9128828e3cb6)
![20241123_091908](https://github.com/user-attachments/assets/7182258b-15f5-496b-b39a-1a134d91831a)

Ooooops, what did i do? Oh shit, did I already break my new toy? Two attemps of resolder the IC brought it back to life again. Manufacturer need to work on their reflow oven process?
![20241123_075507](https://github.com/user-attachments/assets/b4da7686-5e9c-4cc5-8818-eb79d6c6c533)

## Tests
I have so far been running the docker together with my ssc338q on my RC car and focusing on 40Mhz mode which suites the little 8733bu just fine. There is some caveats, for TX LDPC need to be disabled during MCS2, otherwise the stream will break up badly. LDPC works fine for MCS0 and MCS1 though. 8733bu cannot RX with LDPC, but that is only an issue if you are using them on groundstation, or have tunnel or bidirectional mavlink setup. In that case, disable LDPC on your groundstation and it will happily recieve the packets. I could not quanitify any issues with running without LDPC, but im sure in a noisy enviroment it will help to have it enabled.
![photo_2024-11-24_20-28-40](https://github.com/user-attachments/assets/a0b00829-7628-4740-881e-ee6d30aae80b)
https://github.com/user-attachments/assets/ffde9d5b-6be6-4b17-9ba2-369b0562aaa2

I also did test with 40mhz mode running up to 17mbit in MCS1, which was working very well. The range is not very good with omnidirectional antennas, but with a 14dbi directional antenna, I got decent range and very good reception.

## Summary / Observations / Opinions
So far, the docker has met and exceeded my expectations. Its really nice to have a package for a WLAN, BEC and ability to use OpenIPC MIPI cameras. I will try my IMX335 from Mario AIO soon but dont expect any suprises there.
I think this will work very well for a RC car, Planes, and larger drones where you want to experiment and want to have access to all the features of an readily accessible Anjoy ssc338q boarxc (Microphone/speaker, Ethernet, 3 UARTS etcetera). Its also the perfect development board you can mess around with and try new things instead of breaking your AIO's.

I hope the XT30 connector issue will be resolved, and some Quality Control to be established in the manufacturing; IC´s should not pop off by themselves, although it was a easy enough solder job to fix it.
I wish there were some different length flat cables included or available, the one which comes standard with Anjoy board is slightly awkward, since the Docker has changed orientation of the flat cable connector. Preferably i think there should have been one about 0,5-1cm shorter to make it possible to mount the boards more flush together.

I also think there should be a way to disable/disconnect the soldered on rtl8733bu, in case I want to use the external USB port, which by the way has double power cables out and should have no problem driving a rtl8812eu or 8812au. Just remember to change coltage selector (solder blob) to a suitable voltage (3.3/5V).



