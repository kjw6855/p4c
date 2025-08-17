#!/bin/bash
set -m
BASEDIR=$(dirname $0)
AGENT_DIR="/tmp/p4testgen"
P4PROG_PATH="$AGENT_DIR/target.p4"
CUR_PID=0
p4testgen_param_list=("$@")
p4c_params=""

trap p4testgen_shutdown SIGUSR1
trap all_shutdown SIGINT SIGTERM EXIT

is_alive() {
    local pid=$1
    # Check if the process exists and is not a zombie
    if kill -0 "$pid" 2>/dev/null; then
        # Now, check if it's a zombie. A zombie process is technically 'alive'
        # to 'kill -0', so we need this additional check.
        if ps -p "$pid" -o stat= | grep -q 'Z'; then
            return 1 # False: It's a zombie process
        else
            return 0 # True: It's alive and not a zombie
        fi
    else
        return 1 # False: Process does not exist
    fi
}

wait_p4testgen()
{
    # Start a background process to monitor the log and send a signal
    tail -f "$LOG_FILE" | grep -E -m 1 "Server listening on .*5005$ID" & TAIL_PID=$!

    # Clean up the background tail process and the FIFO
    wait "$TAIL_PID"
}

p4testgen_shutdown()
{
    if [[ $CUR_PID -ne 0 ]]; then
        echo "terminate p4testgen"
        kill -9 $CUR_PID
        while is_alive "$CUR_PID"; do :; done
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
    p4c_orig_program="$1"
    p4c_params+=" $P4PROG_PATH"
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

# Copy the program to target.p4
cp $p4c_orig_program $P4PROG_PATH

# Iterate through the array to find and replace the argument.
for i in "${!p4testgen_param_list[@]}"; do
  # Check if the current argument ends with ".p4".
  # The pattern ".*.p4" matches any string ending with ".p4".
  if [[ "${p4testgen_param_list[$i]}" =~ .*\.p4$ ]]; then
    # Replace the argument with "tmp.p4".
    p4testgen_param_list[$i]=$P4PROG_PATH
    break # We found the file, so we can exit the loop.
  fi
done

LOG_FILE="$AGENT_DIR/p4testgen$ID.log"
PIPE_READ_FILE="$AGENT_DIR/p4fuzzer_to_agent$ID"
PIPE_WRITE_FILE="$AGENT_DIR/p4agent_to_fuzzer$ID"
# Create the named pipe if it doesn't exist
if [[ ! -p "$PIPE_READ_FILE" ]]; then
    mkfifo "$PIPE_READ_FILE"
fi
if [[ ! -p "$PIPE_WRITE_FILE" ]]; then
    mkfifo "$PIPE_WRITE_FILE"
fi

INIT=1
MUTATED=0

while [ True ];
do
    if [ $INIT -eq 1 ]; then
        echo "Initalize seed P4 program compilation"
        compile_p4_program
        INIT=0
    fi

    echo "Launching p4testgen..."
    $BASEDIR/p4testgen "${p4testgen_param_list[@]}" > "$LOG_FILE" 2>&1 & CUR_PID=$!
    wait_p4testgen
    echo $CUR_PID > $AGENT_DIR/$ID

    if [ $MUTATED -eq 1 ]; then
        echo "done" > $PIPE_WRITE_FILE
        MUTATED=0
    fi
    echo "p4testgen starts (PID: $CUR_PID)"

    # Wait for the process to exit using a non-blocking check
    # The 'wait' command inside a subshell will not block the main loop
    while is_alive "$CUR_PID"; do
        # Use 'read' with a 1-second timeout
        message=`timeout 1 head -n 1 $PIPE_READ_FILE`
        TIMEOUT_STATUS=$?

        if [ $TIMEOUT_STATUS -eq 124 ]; then
            continue

        elif [ $TIMEOUT_STATUS -eq 0 ]; then
            # This block runs only if a message was received

            echo "Received message: $message"
            if [[ "$message" == "mutate" ]]; then
                status_code=1
                while [ $status_code -ne 0 ]; do
                    echo "Mutate and Compile P4 file"
                    mutate_p4_program
                    compile_p4_program
                    status_code=$?
                    MUTATED=1
                done
                p4testgen_shutdown
                break
            elif [[ "$message" == "update" ]]; then
                echo "Update P4 agent"
                p4testgen_shutdown
                break
            fi
        else
            echo "An error occurred."
            break
        fi
    done

    # At this point, the process has exited
    echo "p4testgen has stopped. Restart it again."
    CUR_PID=0
done
