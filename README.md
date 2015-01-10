# mcastng
mcastng is yet another multicast (udp) to unicast (http) proxy.
It uses Netgraph Framework to do the job and seems to be fast.

#Installing and usage
make && make install
cp mcastng.cfg-sample /usr/local/etc/
echo "mcastng_enable=\"YES\"" >> /etc/rc.conf

/usr/local/etc/rc.d/mcastng start
Sample config below: 

[global]
# This name will be used to name hubs e.g. hub0, hub1, hub2 and so on
basename = hub
# This is an IP address which assigned to multicast interface if you don`t want to write for each server one
mifsrc = 192.168.22.33
[servers]
#IP #PORT #IP #PORT 
vlan9@239.125.10.3:1234	0.0.0.0:9100
239.125.10.10:1234 0.0.0.0:9102  # Will use mifsrc to determin where to send igmp join message
