#!/bin/bash
set -m
BASEDIR=$(dirname $0)
AGENT_DIR="/tmp/p4testgen"
CUR_PID=0
p4testgen_param_list=("$@")
p4c_params=""

trap p4testgen_shutdown SIGUSR1
trap all_shutdown SIGINT SIGTERM EXIT

p4testgen_shutdown()
{
    if [[ $CUR_PID -ne 0 ]]; then
        echo "terminate p4testgen"
        kill -9 $CUR_PID
        while kill -0 $CUR_PID 2>/dev/null; do :; done
    fi
}

all_shutdown()
{
    if [[ $CUR_PID -ne 0 ]]; then
        echo "terminate p4testgen"
        kill -9 $CUR_PID
    fi
    exit 1
}

mutate_p4_program()
{
    echo "Mutate p4 program with $p4c_params"
    $BASEDIR/p4smith $p4c_params
}

compile_p4_program()
{
    echo "Compile p4 program into $AGENT_DIR"
    $BASEDIR/p4c-bm2-ss $p4c_params --toJSON tmp-ir.json --Wdisable=unsupported -o $AGENT_DIR/bmv2.json --p4runtime-files $AGENT_DIR/p4info.txt
    return $?
}

ID=1
for arg in $@
do
    if [[ $arg == 50051 ]]; then
        ID=1
    elif [[ $arg == 50052 ]]; then
        ID=2
    elif [[ $arg == 50053 ]]; then
        ID=3
    fi
done

# Loop through the provided arguments
while (( "$#" )); do
case "$1" in
  # Parameters
  --target|--std|--arch|-I)
    p4c_params+=" $1"
    shift
    p4c_params+=" $1"
    shift
    ;;
  # The main P4 file argument
  *.p4)
    p4c_params+=" $1"
    shift
    ;;
  *)
    shift
    ;;
esac
done

if [[ ! -d $AGENT_DIR ]]; then
    mkdir $AGENT_DIR
fi
echo $$ > $AGENT_DIR/$ID

PIPE_FILE="/tmp/p4testgen/p4agent_pipe"
# Create the named pipe if it doesn't exist
if [[ ! -p "$PIPE_FILE" ]]; then
    mkfifo "$PIPE_FILE"
fi

while [ True ];
do
#    $BASEDIR/p4testgen $@ &
#    CUR_PID=$!
#    echo "p4testgen starts (PID:$CUR_PID)"
#    wait $CUR_PID
#    echo "p4testgen has stopped ..."
#    CUR_PID=0
#    sleep 1
#    echo "Restart p4testgen"

    echo "Compiling seed P4 program"
    compile_p4_program

    echo "Launching p4testgen..."
    $BASEDIR/p4testgen "${p4testgen_param_list[@]}" &
    CUR_PID=$!
    echo "p4testgen starts (PID: $CUR_PID)"

    # Wait for the process to exit using a non-blocking check
    # The 'wait' command inside a subshell will not block the main loop
    while kill -0 $CUR_PID 2>/dev/null; do
        # Use 'read' with a 1-second timeout
        if read -t 1 -r message < "$PIPE_FILE"; then
            # This block runs only if a message was received
            echo "Received message: $message"
            if [[ "$message" == "mutate" ]]; then
                status_code=1
                while [ $status_code -ne 0 ]; do
                    echo "Mutate and Compile P4 file"
                    mutate_p4_program
                    compile_p4_program
                    status_code=$?
                done
                p4testgen_shutdown
                break
            fi
        fi
    done

    # At this point, the process has exited
    echo "p4testgen has stopped. Restart it again."
    CUR_PID=0
done
