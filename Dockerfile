FROM ubuntu:18.04

RUN apt-get update

RUN apt-get install -y git g++ make libssl-dev libgflags-dev libprotobuf-dev libprotoc-dev protobuf-compiler libleveldb-dev libsnappy-dev libgoogle-perftools-dev gdb
RUN apt-get install -y cmake libgtest-dev && cd /usr/src/gtest && cmake . && make && mv libgtest* /usr/lib/

RUN apt-get install -y openssh-server
RUN mkdir /var/run/sshd
RUN sed -ri 's/^#PermitRootLogin\s+.*/PermitRootLogin yes/' /etc/ssh/sshd_config
RUN sed -ri 's/UsePAM yes/#UsePAM yes/g' /etc/ssh/sshd_config

RUN apt-get install -y rsync
RUN sed -ri 's/RSYNC_ENABLE=false/RSYNC_ENABLE=true/g' /etc/default/rsync
COPY rsync.conf /etc

RUN echo 'root:1' |chpasswd

RUN mkdir /root/sync

COPY entrypoint.sh /sbin
RUN chmod +x /sbin/entrypoint.sh
ENTRYPOINT [ "/sbin/entrypoint.sh" ]