echo "TEST: 01-main"
./tests/01-main 
echo "------------------------------------------------"
echo "TEST: 02-switch"
./tests/02-switch
echo "------------------------------------------------"
echo "TEST: 11-join"
./tests/11-join
echo "------------------------------------------------"
echo "TEST: 12-join-main"
./tests/12-join-main
echo "------------------------------------------------"
echo "TEST: 13-join-cascade"
./tests/13-join-cascade 200
echo "------------------------------------------------"
echo "TEST: 21-create-many 40000"
./tests/21-create-many 40000
echo "------------------------------------------------"
echo "TEST: 22-create-many-recursive 4000"
./tests/22-create-many-recursive 4000
echo "------------------------------------------------"
echo "TEST: 31-switch-many 400 800"
./tests/31-switch-many 400 800
echo "------------------------------------------------"
echo "TEST: 32-switch-many-join 400 800"
./tests/32-switch-many-join 400 800
echo "------------------------------------------------"
echo "TEST: 51-fibonacci 23"
./tests/51-fibonacci 23
echo "------------------------------------------------"
echo "TEST: 52-array-sum 1000"
./tests/52-array-sum 1000
echo "------------------------------------------------"
echo "TEST: 53-quicksort 100"
./tests/53-quicksort 100
echo "------------------------------------------------"
echo "TEST: 54-mergesort 100"
./tests/54-mergesort 100
echo "------------------------------------------------"
echo "TEST: 55-increment 90000"
./tests/55-increment 90000
echo "------------------------------------------------"
echo "TEST: 56-cancel"
./tests/56-cancel
