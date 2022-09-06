#!/bin/sh
cd ..
if [ "${MAKE}" = "" ] ;then
	MAKE=make
fi
# ${MAKE} clean launchd-debug
cd sa-wrapper
rm -f *.out *.err
../launchd -fv &
echo $!
sleep 2
tail -f ~/.local/share/launchd/launchd.log &
./setup-plist.rb
sleep 2
message=`curl --http0.9 http://localhost:8088`
if [ $? -ne 0 ] ; then
        echo fail
        retval=1
else
        if [ "x$message" = "xhello world" ] ; then
                echo success
                retval=0
        else
                echo "fail (mismatching message)"
                retval=2
        fi
fi
set -x
kill `cat ~/.local/share/launchd/run/launchd.pid`
set +x
# kill %2
echo "response from the server"
echo $message
echo "-----"
echo "standard output of the job:"
cat test-wrapper.out
echo "-----"
echo "standard error of the job:"
cat test-wrapper.err
echo "-----"
exit $retval
