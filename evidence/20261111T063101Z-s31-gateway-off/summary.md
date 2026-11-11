# S31 gateway-off test — 2026-11-11

The S31 was reachable at DHCP address 192.168.88.138. The identified
PC voice gateway process (PID 15580) was stopped, and UDP 5000 was
verified unbound before and after the observation window.

With the gateway off, Direct Cloud remained active, fallback was inactive,
and both Mistral sessions were connected. After the first gateway-off
snapshot, the board's playback packet counter increased from 471 to 595.
First audio was measured at 5,151 ms and playback drops remained zero.
The completed direct turn counter stayed at zero. A full audible reply
still needs operator confirmation. The gateway was left off for this test.
