#!/bin/bash
if [ $# -eq 3 ] 
then
    rm $1.dat
    for i in `seq 1 $2 $3` 
    do
	let "p=100*$i/$3"
	printf "$i\r" > /dev/stderr
	printf "$i\t" >> $1.dat;
	time=`date +%s%N`
	./$1 $i > /dev/null
	acttime=`date +%s%N`
	let "newtime=$acttime-$time"
	printf "$newtime\n" >> $1.dat
    done
elif [ $# -eq 5 ]
then
    rm $1.dat
    for i in `seq 1 $2 $3` 
    do
	let "p=10*$i/$3"
	printf "$i\r" > /dev/stderr 
	for j in `seq 1 $4 $5`
	do 
	    printf "$i\t$j\t" >> $1.dat;
	    time=`date +%s%N`
	    ./$1 $i $j > /dev/null
	    acttime=`date +%s%N`
	    let "newtime=$acttime-$time"
	    printf "$newtime\n" >> $1.dat
	done
    done
else 
    echo "Usage: $0 executable istep imax [jstep jmax]"
    exit 1
fi
exit 0