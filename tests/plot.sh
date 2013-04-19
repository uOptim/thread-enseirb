#!/bin/bash
if [ $# -eq 2 ] 
then
    for i in `seq 1 $2` 
    do
	let "test=$i%10"
	if [ $test -eq 0 ] 
	then printf "."
	fi
	echo -e "$i\t" >> tmp.dat;
	/usr/bin/time -f"%e" ./$1 $i 2>> tmp.dat;
    done
    tr "\n$" "\t" < tmp.dat > tmp2.dat;
    sed 's/\([0-9]*\t[0-9]*\.[0-9]*\s\)/\1\n/g' tmp2.dat > $1.dat
    rm tmp*.dat    
elif [ $# -eq 3 ]
then
    for i in `seq 1 $2` 
    do 
	let "test=$i%10"
	if [ $test -eq 0 ]
	then printf "."
	fi
	for j in `seq 1 $3`
	do echo -e "$i\t$j\t" >> tmp.dat;
	    /usr/bin/time -f"%e" ./$1 $i $j 2>> tmp.dat;
	done
    done
    tr "\n$" "\t" < tmp.dat > tmp2.dat;
    sed 's/\([0-9]*\t[0-9]*\t[0-9]*\.[0-9]*\s\)/\1\n/g' tmp2.dat > $1.dat
    rm tmp*.dat    
else 
    echo "Usage: $0 executable imax [jmax]"
    exit 1
fi
exit 0