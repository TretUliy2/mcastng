# mcastng
mcastng is yet another multicast (udp) to unicast (http) proxy.
It uses Netgraph Framework to do the job and seems to be fast.
This piece of software doing not much. It just creates Netgraph nodes for each server (ng_hub for multiply traffic, ng_ksocket for upstream, ng_ksocket for clients connection serve) and connects new clients to ng_hub.
#Building and Installing
git clone https://github.com/TretUliy2/mcastng.git
cd mcastng
make && make install
cp mcastng.cfg-sample /usr/local/etc/mcastng.cfg
echo "mcastng_enable=\"YES\"" >> /etc/rc.conf

/usr/local/etc/rc.d/mcastng start

