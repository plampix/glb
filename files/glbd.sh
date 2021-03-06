#!/bin/sh
#
# Author: Daniel (thehazard@gmail.com), Codership Oy (info@codership.com)
# Copyright: this script is Public Domain.
#
# This script is provided as is, without any guarantees, including that
# of suitability for any purpose.
#
# glbd          Start/Stop the Galera Load Balancer daemon.
#
# processname: glbd
# chkconfig: 2345 90 60
# description: GLB is a TCP load balancer similar to Pen. \
#              It lacks most of advanced Pen features, as \
#              the aim was to make a user-space TCP proxy which is \
#              as fast as possible. It can utilize multiple CPU cores. \
#              A list of destinations can be configured at runtime. \
#              Destination "draining" is supported. It features \
#              weight-based connection balancing.

### BEGIN INIT INFO
# Provides: glbd-default
# Required-Start: $local_fs $network
# Required-Stop: $local_fs
# Default-Start:  2 3 4 5
# Default-Stop: 9 0
# Short-Description: run glbd daemon
# Description: GLB is a TCP load balancer similar to Pen.
### END INIT INFO

DAEMON=glbd
INSTANCE=default
GLBINSTANCE=$DAEMON-$INSTANCE
EXEC_PATH=/usr/local/sbin:/usr/sbin
PID_FILE="/var/run/$GLBINSTANCE.pid"
CONTROL_FIFO="/var/run/$GLBINSTANCE.fifo"
THREADS=4
NC_OPTIONS="-q 1"

if [ -f /etc/redhat-release ]
then
	config=/etc/sysconfig/$GLBINSTANCE
else
	config=/etc/default/$GLBINSTANCE
fi

. $config

LISTEN_PORT=$(echo $LISTEN_ADDR | awk -F ':' '{ print $2 }')
[ -z "$LISTEN_PORT" ] && LISTEN_PORT=$LISTEN_ADDR

if [ -n "$CONTROL_ADDR" ]; then
	CONTROL_PORT=$(echo $CONTROL_ADDR | awk -F ':' '{ print $2 }')
	if [ -n "$CONTROL_PORT" ]; then # CONTROL_ADDR has both address and port
		CONTROL_IP=$(echo $CONTROL_ADDR | awk -F ':' '{ print $1 }')
	else                            # CONTROL_ADDR contains only port
		CONTROL_PORT=$CONTROL_ADDR
		CONTROL_IP="127.0.0.1"
	fi
else
	CONTROL_IP=""
	CONTROL_PORT=""
fi

wait_for_connections_to_drop() {
	sleep 1s
	return 0
	while (netstat -na | grep -m 1 ":$LISTEN_PORT " > /dev/null); do
		echo "[`date`] $GLBINSTANCE: waiting for lingering sockets to clear up..."
		sleep 1s
	done;
	return 0
}

stop() {
	[ -f "$PID_FILE" ] && PID=$(cat $PID_FILE) || PID=""
	if [ -z "$PID" ]; then
		echo "No valid PID file found at '$PID_FILE'"
		return
	fi
	echo -n "[`date`] $GLBINSTANCE: stopping... "
	kill $PID > /dev/null
	if [ $? -ne 0 ]; then
		echo "failed."
		return
	fi
	echo "done."
	rm $PID_FILE
	rm $CONTROL_FIFO
}

start() {
	exec=$( PATH=$EXEC_PATH:/usr/bin:/bin which $DAEMON | \
	        grep -E $(echo $EXEC_PATH | sed 's/:/|/') )
	if [ -z "$exec" ]; then
		echo "[`date`] '$DAEMON' not found in $EXEC_PATH."
		exit 1
	fi
	[ -f "$PID_FILE" ] && PID=$(cat $PID_FILE) || PID=""
	if [ -n "$PID" ] ; then
		echo "[`date`] $GLBINSTANCE: already running (PID: $PID)...";
		exit 1
	fi
	if [ -z "$LISTEN_PORT" ]; then
		echo "[`date`] $GLBINSTANCE: no port to listen at, check configuration.";
		exit 1
	fi
	echo "[`date`] $GLBINSTANCE: starting..."
	wait_for_connections_to_drop
	rm -rf $CONTROL_FIFO > /dev/null
	GLBD_OPTIONS="--fifo=$CONTROL_FIFO --threads=$THREADS --daemon $OTHER_OPTIONS"
	[ -n "$MAX_CONN" ] && GLBD_OPTIONS="$GLBD_OPTIONS --connections=$MAX_CONN"
	[ -n "$CONTROL_ADDR" ] && GLBD_OPTIONS="$GLBD_OPTIONS --control=$CONTROL_ADDR"
	eval $exec $GLBD_OPTIONS $LISTEN_ADDR $DEFAULT_TARGETS
	PID=`ps axw | grep "$exec .* $LISTEN_ADDR $DEFAULT_TARGETS" | grep -v grep | awk '{print $1}'`
	if [ $? -ne 0 ]; then
		echo "[`date`] $GLBINSTANCE: failed to start."
		exit 1
	fi
	echo "[`date`] $GLBINSTANCE: started, pid=$PID"
	echo $PID > "$PID_FILE"
	exit 0
}

restart() {
	echo "[`date`] $GLBINSTANCE: restarting..."
	stop
	start
}

getinfo() {
	if [ -z "$CONTROL_PORT" ]; then
		echo "Port for control communication is not configured."
		exit 1
	fi
	echo getinfo | nc $NC_OPTIONS $CONTROL_IP $CONTROL_PORT && exit 0
	echo "[`date`] $GLBINSTANCE: failed to query 'getinfo' from '$CONTROL_ADDR'"
	exit 1
}

getstats() {
	if [ -z "$CONTROL_PORT" ]; then
		echo "Port for control communication is not configured."
		exit 1
	fi
	echo getstats | nc $NC_OPTIONS $CONTROL_IP $CONTROL_PORT && exit 0
	echo "[`date`] $GLBINSTANCE: failed to query 'getstats' from '$CONTROL_ADDR'"
	exit 1
}

add() {
	if [ -z "$CONTROL_PORT" ]; then
		echo "Port for control communication is not configured."
		exit 1
	fi
	if [ "$1" = "" ]; then
		echo "Usage: $0 add <ip>:<port>[:<weight>]"
		exit 1
	fi
	if [ "`echo "$1" | nc $NC_OPTIONS $CONTROL_IP $CONTROL_PORT`" = "Ok" ]; then
		echo "[`date`] $GLBINSTANCE: added '$1' successfully"
		#getinfo
		exit 0
	fi
	echo "[`date`] $GLBINSTANCE: failed to add target '$1'."
	exit 1
}

remove() {
	if [ -z "$CONTROL_PORT" ]; then
		echo "Port for control communication is not configured."
		exit 1
	fi
	if [ "$1" = "" ]; then
		echo "Usage: $0 remove <ip>:<port>"
		exit 1
	fi
	if [ "`echo "$1:-1" | nc $NC_OPTIONS $CONTROL_IP $CONTROL_PORT`" = "Ok" ]; then
		echo "[`date`] $GLBINSTANCE: removed '$1' successfully"
		#getinfo
		exit 0
	fi
	echo "[`date`] $GLBINSTANCE: failed to remove target '$1'."
	exit 1
}

drain() {
	if [ -z "$CONTROL_PORT" ]; then
		echo "Port for control communication is not configured."
		exit 1
	fi
	if [ "$1" = "" ]; then
		echo "Usage: $0 drain <ip>:<port>"
		exit 1
	fi
	if [ "`echo "$1:0" | nc $NC_OPTIONS $CONTROL_IP $CONTROL_PORT`" = "Ok" ]; then
		echo "[`date`] $GLBINSTANCE: '$1' was set to drain connections"
		#getinfo
		exit 0
	fi
	echo "[`date`] $GLBINSTANCE: failed to set '$1' to drain."
	exit 1
}

case $1 in
	start)
		start
	;;
	stop)
		stop
	;;
	restart)
		restart
	;;
	getinfo)
		getinfo
	;;
	getstats)
		getstats
	;;
	status)
		getinfo
	;;
	add)
		add $2
	;;
	remove)
		remove $2
	;;
	drain)
		drain $2
	;;
	*)
		echo "Usage: $0 {start|stop|restart|status|getstats|getinfo|add|remove|drain}"
	exit 2
esac
