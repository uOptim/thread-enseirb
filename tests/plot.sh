#!/bin/bash
if [ $# -eq 2 ] 
then
    for i in `seq 1 $2` 
    do
	let "test=$i%10"
	if [ $test -eq 0 ] 
	then printf "."
	fi
	printf "$i\t" >> $1.dat;
	time=`date +%s%N`
	./$1 $i $j;
	acttime=`date +%s%N`
	let "newtime=$acttime-$time"
	printf "$newtime\n" >> $1.dat
    done
elif [ $# -eq 3 ]
then
    for i in `seq 1 $2` 
    do 
	let "test=$i%10"
	if [ $test -eq 0 ]
	then printf "."
	fi
	for j in `seq 1 $3`
	do 
	    printf"$i\t$j\t" >> $1.dat;
	    time=`date +%s%N`
	    ./$1 $i $j;
	    acttime=`date +%s%N`
	    let "newtime=$acttime-$time"
	    printf "$newtime\n" >> $1.dat
	done
    done
else 
    echo "Usage: $0 executable imax [jmax]"
    exit 1
fi
exit 0