# mcastng
mcastng is yet another multicast (udp) to unicast (http) proxy.
It uses Netgraph Framework to do the job and seems to be fast.

#Installing and usage
make && make install
cp mcastng.cfg-sample /usr/local/etc/
echo "mcastng_enable=\"YES\"" >> /etc/rc.conf

/usr/local/etc/rc.d/mcastng start

