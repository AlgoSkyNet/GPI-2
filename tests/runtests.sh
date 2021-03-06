#!/bin/sh

GPI2_TSUITE_MFILE=machines
GASPI_RUN="../bin/gaspi_run"
GASPI_CLEAN="../bin/gaspi_cleanup"
TESTS=`ls bin`
NUM_TESTS=0
TESTS_FAIL=0
TESTS_PASS=0
TESTS_TIMEOUT=0
TESTS_SKIPPED=0
Results=1 #if we want to look at the results/output
Time=1
opts_used=0
LOG_FILE=runtests_$(date -Idate).log

MAX_TIME=600

#Functions
exit_timeout(){
    echo "Stop this program"
    trap - TERM INT QUIT
    $GASPI_CLEAN  -m ${GPI2_TSUITE_MFILE}
    kill -9 $TPID > /dev/null 2>&1
    killall -9 sleep > /dev/null 2>&1
    sleep 1

    trap exit_timeout TERM INT QUIT
    TESTS_FAIL=$(($TESTS_FAIL+1))
    printf '\033[31m'"KILLED\n"

    #reset terminal to normal
    tput sgr0
}

run_test(){
    TEST_ARGS=""
    #check definitions file for particular test
    F="${1%.*}"
    if [ -r defs/${F}.def ]; then
	printf "%45s: " "$1 [${F}.def]"
	SKIP=`gawk '/SKIP/{print 1}' defs/${F}.def`
	if [ -n "$SKIP" ]; then
	    printf '\033[34m'"SKIPPED\n"
	    TESTS_SKIPPED=$((TESTS_SKIPPED+1))
   #reset terminal to normal
	    tput sgr0
	    return
	fi

	TEST_ARGS=`gawk 'BEGIN{FS="="} /ARGS/{print $2}' defs/${F}.def`
    else

    #check definitions file (default)
	if [ -r defs/default.def ]; then
	    printf "%45s: " "$1 [default.def]"
	    TEST_ARGS=`gawk 'BEGIN{FS="="} /NETWORK/{print $2}' defs/default.def`
	    TEST_ARGS="$TEST_ARGS "" `gawk 'BEGIN{FS="="} /TOPOLOGY/{print $2}' defs/default.def`"
	else
	    printf "%45s: " "$1"
	fi
    fi

    if [ $Results = 0 ] ; then
	$GASPI_RUN -m ${GPI2_TSUITE_MFILE} $PWD/bin/$1 > results/$1-$(date -Idate).dat 2>&1 &
	PID=$!
    else
	echo "=================================== $1 ===================================" >> $LOG_FILE 2>&1 &
	$GASPI_RUN -m ${GPI2_TSUITE_MFILE} $PWD/bin/$1 $TEST_ARGS >> $LOG_FILE 2>&1 &
	PID=$!
    fi

    TIMEDOUT=0
    if [ $Time = 1 ] ; then
	export PID
	(sleep $MAX_TIME; kill -9 $PID;) &
	TPID=$!
   #wait test to finish
	wait $PID 2>/dev/null
    fi

    TEST_RESULT=$?
    kill -0 $TPID || let "TIMEDOUT=1"
    if [ $TIMEDOUT = 1 ];then
	TESTS_TIMEOUT=$(($TESTS_TIMEOUT+1))
	printf '\033[33m'"TIMEOUT\n"
	$GASPI_CLEAN -m ${GPI2_TSUITE_MFILE}
    else
	if [ $TEST_RESULT = 0 ]; then
	    TESTS_PASS=$(($TESTS_PASS+1))
	    printf '\033[32m'"PASSED\n"
	else
	    TESTS_FAIL=$(($TESTS_FAIL+1))
	    printf '\033[31m'"FAILED\n"
	    $GASPI_CLEAN -m ${GPI2_TSUITE_MFILE}
	fi
    fi

   #reset terminal to normal
    tput sgr0

    if [ $Time = 1 ] ; then
	if [ $TIMEDOUT = 0 ];then
	    kill $TPID  > /dev/null 2>&1
	fi
    fi
}

trap exit_timeout TERM INT QUIT

#Script starts here
OPTERR=0
while getopts "e:vtn:m:o:" option ; do
    case $option in
	e ) MAX_TIME=${OPTARG}; opts_used=$(($opts_used + 2));;
	v ) Results=1;opts_used=$(($opts_used + 1));echo "" > $LOG_FILE ;;
	t ) Time=0;;
	n ) GASPI_RUN="${GASPI_RUN} -n ${OPTARG}";opts_used=$(($opts_used + 2));;
	m ) GPI2_TSUITE_MFILE=${OPTARG};opts_used=$(($opts_used + 2));;
	o ) LOG_FILE=${OPTARG};opts_used=$(($opts_used + 2));;
	\?) FAILED_OPTION=$(($OPTIND-1));echo;echo "Unknown option (${!FAILED_OPTION})";echo;exit 1;;
    esac
done

#want to run particular tests
if [ $(($# - $opts_used)) != 0 ]; then
FIRST_TEST=$(($# - $(($# - $opts_used)) + 1))
TESTS="${!FIRST_TEST}"
for ((i=$FIRST_TEST+1;i<=$#;i++));
do
    TESTS="$TESTS ${!i}"
done
fi

#check machine file
if [ ! -r $GPI2_TSUITE_MFILE ]; then
    echo
    echo "File ($GPI2_TSUITE_MFILE) not found."
    echo "You can create a machine file called *machines* in the same directory of this script."
    echo "OR provide a valid machinefile using the (-m) option."
    echo
    exit 1
fi
#check if tests were compiled (if exist)
if [ "$TESTS" = "" ]; then
    printf "\nNo tests found. Did you type make before?\n"
    exit 1
fi

which numactl > /dev/null
if [ $? = 0 ]; then
    MAX_NUMA_NODES=`numactl --hardware|grep available|gawk '{print $2}'`
    HAS_NUMA=1
fi

if [ "$HAS_NUMA" = "1" ]; then
    GASPI_RUN="${GASPI_RUN} -N"
fi
#run them
for i in $TESTS
do
    if [ "$i" = "-v" ]; then
	continue
    fi
    if [ `find $PWD/bin/ -iname $i ` ]; then
	run_test $i
	NUM_TESTS=$(($NUM_TESTS+1))
	sleep 1
    else
	echo "Test $i does not exist."
    fi
done

killall sleep 2>/dev/null

echo
echo -e "Run $NUM_TESTS tests:\n$TESTS_PASS passed\n$TESTS_FAIL failed\n$TESTS_TIMEOUT timed-out\n$TESTS_SKIPPED skipped\nTimeout $MAX_TIME (secs)\n"

exit 0
