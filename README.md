# mcastng
mcastng is yet another multicast (udp) to unicast (http) proxy uses mostly by IPTV proxys.<br>
It uses Netgraph Framework to do the job and seems to be fast.<br>
This piece of software doing not much. It just creates Netgraph nodes for each server (ng_hub for multiply traffic, ng_ksocket for upstream, ng_ksocket for clients connection serve) and connects new clients to ng_hub. <br> Another one thing is to kill useless ng_ksocket nodes (when client is dissconnected ng_ksocket node is hanging in kernel and needs to be shutdown manualy)
#Building and Installing
    git clone https://github.com/TretUliy2/mcastng.git
    cd mcastng
    make && make install
    cp mcastng.cfg-sample /usr/local/etc/mcastng.cfg
    echo "mcastng_enable=\"YES\"" >> /etc/rc.conf

    /usr/local/etc/rc.d/mcastng start

